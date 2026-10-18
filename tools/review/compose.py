"""Build a review batch page from recorded jobs (Session 8, task 4): side-by-side clips, still sheets with 100% crops,
checks, and out/harness/review/<batch>/index.html (relative links only).

    py tools\\review\\compose.py --batch batch1            build everything that has recordings
    py tools\\review\\compose.py --batch batch1 --check    also sample frames of every clip (black / frozen check)
    py tools\\review\\compose.py --batch batch1 --arm-map ""   show the 4k jobs (60 fps clips) as the 4K arm; the default
                                                           4k=4k30 shows a route's 4k30 job (30 fps clip) when it has one

Inputs: out/harness/review/<batch>/runs/<route>__<arm>/job.json (+ clip_*.mp4, still_*.png) written by record.py,
and for the Create a Logo sheet the existing captures of runs 146 (defaults) and 175 (4K preset).
Outputs (gitignored, game-derived): clips/*.mp4 (side by side, labelled, h264_nvenc), sheets/*.png, checks/*.png,
checks.json, index.html.
"""
import argparse
import html
import json
import os
import subprocess
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
REVIEW = os.path.join(REPO, 'out', 'harness', 'review')
CAPTURES = os.path.join(REPO, 'out', 'build', 'win-amd64-release', 'captures')
FONT = 'C:/Windows/Fonts/arialbd.ttf'
FONT_FF = 'C\\:/Windows/Fonts/arialbd.ttf'   # ffmpeg filter syntax (escaped drive colon)

ARM_SHORT = {'default': 'Default (4x MSAA)', '1440p': '1440p preset', '4k': '4K preset',
             '4k30': '4K preset (clip 30 fps)', '4khalf': '4K preset (clip 1080p)'}


def arm_short(arm):
    return ARM_SHORT.get(arm, arm)


def apply_arm_map(jobs, arm_map):
    """--arm-map 4k=4k30: a recorded <route>__4k30 job stands in for the item arm '4k' (its own arm stays in the
    job, so labels say which job is shown). Routes without a 4k30 job keep their 4k job."""
    view = dict(jobs)
    for item_arm, job_arm in arm_map.items():
        for (route, arm), j in jobs.items():
            if arm == job_arm:
                view[(route, item_arm)] = j
    return view

# Crop regions (fractions of the frame: x, y, w, h) per still label prefix. Chosen from the default-arm stills; the
# same fractions are cut from every arm, so a region shows the same part of the screen (not always the same
# people: camera variants and animation can differ between runs).
# Exact (route, label) entries first (picked from run 186's default-arm stills), then label prefixes.
REGIONS_EXACT = {
    ('bryan', 'rv_e2_t09p0'): [('crowd', 0.62, 0.33, 0.20, 0.20), ('Bryan + stage', 0.36, 0.30, 0.20, 0.20)],
    ('bryan', 'rv_e2_t14p7'): [('crowd', 0.02, 0.22, 0.20, 0.20), ('barricade + ramp', 0.62, 0.45, 0.20, 0.20)],
    ('bryan', 'rv_e2_t20p0'): [('crowd', 0.02, 0.30, 0.20, 0.20), ('barricade + sign', 0.55, 0.50, 0.20, 0.20)],
    ('bryan', 'rv_match_t10p0'): [('crowd', 0.05, 0.02, 0.20, 0.20), ('ropes', 0.00, 0.55, 0.20, 0.20),
                                  ('wrestlers', 0.38, 0.35, 0.20, 0.20)],
    ('bryan', 'rv_match_t15p0'): [('crowd', 0.55, 0.28, 0.20, 0.20), ('ropes', 0.00, 0.50, 0.20, 0.20),
                                  ('wrestlers', 0.36, 0.30, 0.20, 0.20)],
    ('taker', 'rv_e2_t09p0'): [('stage truss + lights', 0.30, 0.05, 0.20, 0.20),
                               ('crowd up close', 0.25, 0.65, 0.20, 0.20)],
    ('taker', 'rv_e2_t14p7'): [('video board + truss', 0.28, 0.12, 0.20, 0.20), ('crowd', 0.40, 0.55, 0.20, 0.20)],
    ('taker', 'rv_e2_t20p0'): [('Undertaker + smoke', 0.15, 0.15, 0.20, 0.20),
                               ('screen edge', 0.55, 0.10, 0.20, 0.20)],
    ('taker', 'rv_match_t10p0'): [('crowd + sign', 0.10, 0.10, 0.20, 0.20), ('turnbuckle', 0.00, 0.40, 0.20, 0.20),
                                  ('wrestlers', 0.33, 0.42, 0.20, 0.20)],
    ('taker', 'rv_match_t15p0'): [('crowd', 0.30, 0.05, 0.20, 0.20), ('ropes', 0.05, 0.48, 0.20, 0.20),
                                  ('sign', 0.68, 0.20, 0.20, 0.20)],
}
REGIONS = {
    'rv_e2': [('crowd', 0.02, 0.30, 0.20, 0.20), ('edges', 0.40, 0.45, 0.20, 0.20)],
    'rv_match': [('crowd', 0.10, 0.08, 0.20, 0.20), ('ropes', 0.00, 0.55, 0.20, 0.20),
                 ('wrestlers', 0.40, 0.40, 0.20, 0.20)],
    'rv_menu': [('menu text', 0.08, 0.53, 0.20, 0.20), ('logo', 0.15, 0.27, 0.20, 0.20)],
    'rv_caw_t02': [('loading screen', 0.60, 0.05, 0.20, 0.20), ('logo', 0.76, 0.35, 0.20, 0.20)],
    'rv_caw_t28': [('model', 0.52, 0.18, 0.20, 0.20), ('menu text', 0.05, 0.05, 0.20, 0.20)],
    'rv_caw_t33': [('model', 0.52, 0.18, 0.20, 0.20), ('menu text', 0.05, 0.05, 0.20, 0.20)],
    'rv_caw': [('menu text', 0.05, 0.05, 0.20, 0.20)],
    'logo': [('text', 0.05, 0.08, 0.20, 0.20), ('grid', 0.10, 0.30, 0.20, 0.20)],
}


def font(size):
    try:
        return ImageFont.truetype(FONT, size)
    except OSError:
        return ImageFont.load_default()


def label_box(draw, xy, text, size=22):
    f = font(size)
    x, y = xy
    l, t, r, b = draw.textbbox((x + 6, y + 4), text, font=f)
    draw.rectangle((x, y, r + 6, b + 6), fill=(0, 0, 0))
    draw.text((x + 6, y + 4), text, font=f, fill=(255, 230, 80))


def still_sheet(arms, regions, out, title):
    """arms: list of (label, png path). One row of full frames, then one row per crop region: the crop at the
    highest-resolution arm's native size (100%), lower-resolution arms magnified with nearest neighbour."""
    import math
    ims = [(lab, Image.open(p).convert('RGB')) for lab, p in arms]
    maxw = max(im.width for _, im in ims)
    if not regions:
        regions = [('centre', 0.40, 0.40, 0.20, 0.20)]
    # Integer nearest-neighbour factors only (a x1.5 nearest repeats columns 1,2,1,2 and adds stair steps the game
    # never rendered): every crop is magnified to the least common multiple of the arms' crop widths
    # (720p x6, 1440p x3, 4K x2 for 3 arms; x3 / x1 for default vs 4K).
    def crop_w(fw, im):
        return max(int(round(fw * im.width)), 1)

    def target_w(fw):
        t = 1
        for _, im in ims:
            t = t * crop_w(fw, im) // math.gcd(t, crop_w(fw, im))
        return t
    cw = max(target_w(r[3]) for r in regions)
    colw = max(cw, 640)
    rows = []
    # full frames, downscaled to the column width, crop regions outlined
    full = []
    for lab, im in ims:
        fr = im.resize((colw, int(round(colw * im.height / im.width))), Image.LANCZOS)
        d = ImageDraw.Draw(fr)
        for name, fx, fy, fw, fh in regions:
            d.rectangle((fx * fr.width, fy * fr.height, (fx + fw) * fr.width, (fy + fh) * fr.height),
                        outline=(255, 230, 80), width=3)
            d.text((fx * fr.width + 4, fy * fr.height + 2), name, font=font(18), fill=(255, 230, 80))
        label_box(d, (0, 0), f'{lab}, {im.width}x{im.height}')
        full.append(fr)
    rows.append(full)
    for name, fx, fy, fw, fh in regions:
        row = []
        tw = target_w(fw)
        th = 0
        for lab, im in ims:
            x0, y0 = int(fx * im.width), int(fy * im.height)
            w, h = crop_w(fw, im), max(int(round(fh * im.height)), 1)
            c = im.crop((x0, y0, x0 + w, y0 + h))
            mag = max(tw // w, 1)  # integer by construction (tw is a multiple of w)
            c = c.resize((w * mag, h * mag), Image.NEAREST)
            th = max(th, c.height)
            tile = Image.new('RGB', (colw, c.height), (20, 20, 20))
            tile.paste(c, ((colw - c.width) // 2, 0))
            d = ImageDraw.Draw(tile)
            how = '100%' if mag == 1 else f'x{mag} nearest'
            label_box(d, (0, 0), f'{name}: {lab}, {how}', 18)
            row.append(tile)
        rows.append(row)
    W = colw * len(ims)
    H = 44 + sum(max(t.height for t in r) for r in rows)
    sheet = Image.new('RGB', (W, H), (10, 10, 10))
    d = ImageDraw.Draw(sheet)
    d.text((8, 8), title, font=font(26), fill=(255, 255, 255))
    y = 44
    for r in rows:
        for i, t in enumerate(r):
            sheet.paste(t, (i * colw, y))
        y += max(t.height for t in r)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    sheet.save(out, optimize=False, compress_level=6)
    return out


def clip_seconds(path):
    """A clip's duration from its container (fast: no frame count)."""
    r = subprocess.run(['ffprobe', '-v', 'error', '-show_entries', 'format=duration', '-of', 'json', path],
                       capture_output=True, text=True)
    try:
        return float((json.loads(r.stdout or '{}').get('format') or {}).get('duration') or 0)
    except ValueError:
        return 0.0


def side_by_side(clips, out):
    """clips: list of (label, mp4). Each scaled to 1280x720, labelled, stacked left to right. Shorter clips hold their
    last frame until the longest ends (never cut: in-game clips run in real time, so an arm whose game ran slower
    is longer, e.g. run 396's 4K Bryan entrance 19.70 s vs 17.98 s; cutting to the shortest dropped its last
    1.7 s). Returns the list of (label, seconds, padded seconds)."""
    n = len(clips)
    secs = [clip_seconds(p) for _, p in clips]
    longest = max(secs) if secs else 0
    args = ['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y']
    for _, p in clips:
        args += ['-i', p]
    parts = []
    for i, (lab, _) in enumerate(clips):
        txt = lab.replace(':', '\\:').replace("'", '')
        pad = longest - secs[i]
        tpad = f',tpad=stop_mode=clone:stop_duration={pad:.3f}' if pad > 0.02 else ''
        parts.append(f"[{i}:v]setpts=PTS-STARTPTS,fps=60,scale=1280:720,format=yuv420p{tpad},"
                     f"drawtext=fontfile='{FONT_FF}':"
                     f"text='{txt}':x=12:y=10:fontsize=26:fontcolor=yellow:box=1:boxcolor=black@0.7:boxborderw=6[v{i}]")
    parts.append(''.join(f'[v{i}]' for i in range(n)) + f'hstack=inputs={n}:shortest=0[out]')
    args += ['-filter_complex', ';'.join(parts), '-map', '[out]', '-c:v', 'h264_nvenc', '-preset', 'p5',
             '-rc', 'vbr', '-cq', '20', '-b:v', '0', '-pix_fmt', 'yuv420p', '-movflags', '+faststart', out]
    os.makedirs(os.path.dirname(out), exist_ok=True)
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(r.stderr[-1500:])
    return [(lab, round(s, 2), round(longest - s, 2)) for (lab, _), s in zip(clips, secs)]


def probe(path):
    r = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-count_frames', '-show_entries',
                        'stream=width,height,nb_read_frames,pix_fmt,codec_name,avg_frame_rate:format=duration',
                        '-of', 'json', path], capture_output=True, text=True)
    j = json.loads(r.stdout or '{}')
    s = (j.get('streams') or [{}])[0]
    return {'w': s.get('width'), 'h': s.get('height'), 'frames': int(s.get('nb_read_frames') or 0),
            'pix_fmt': s.get('pix_fmt'), 'codec': s.get('codec_name'), 'fps': s.get('avg_frame_rate'),
            'duration': round(float((j.get('format') or {}).get('duration') or 0), 2)}


def sample_frames(mp4, outdir, n=5):
    """n frames spread over the clip: mean luma, and mean abs difference to the previous sample (0 = frozen)."""
    import numpy as np
    info = probe(mp4)
    dur = info['duration'] or 1
    os.makedirs(outdir, exist_ok=True)
    base = os.path.splitext(os.path.basename(mp4))[0]
    parent = os.path.basename(os.path.dirname(mp4))
    if parent != 'clips':
        base = f'{parent}_{base}'  # runs/<route>__<arm>/clip_X.mp4: keep arms apart
    res, prev = [], None
    for k in range(n):
        t = dur * (k + 0.5) / n
        png = os.path.join(outdir, f'{base}_s{k}.png')
        subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', '-ss', f'{t:.2f}', '-i', mp4,
                        '-frames:v', '1', png], capture_output=True)
        if not os.path.exists(png):
            res.append({'t': round(t, 2), 'error': 'no frame'})
            continue
        a = np.asarray(Image.open(png).convert('L'), dtype=np.float32)
        r = {'t': round(t, 2), 'png': os.path.relpath(png, os.path.dirname(outdir)), 'mean_luma': round(float(a.mean()), 1)}
        if prev is not None and prev.shape == a.shape:
            r['diff_prev'] = round(float(np.abs(a - prev).mean()), 2)
        prev = a
        res.append(r)
    return {'probe': info, 'samples': res,
            'black': all(s.get('mean_luma', 0) < 8 for s in res),
            'frozen': all(s.get('diff_prev', 1) < 0.05 for s in res[1:]) if len(res) > 1 else None}


LOGS = os.path.join(REPO, 'out', 'build', 'win-amd64-release', 'logs')


def clip_window_stats(run, name):
    """Guest frame stats between the route's clip_start / clip_stop lines (frames_NNN.csv): with the capture
    running, so this is the capture-overhead check (compare with the arm's fps without capture)."""
    import csv
    import re
    n = f'{int(run):03d}'
    f0 = f1 = None
    try:
        for line in open(os.path.join(LOGS, f'wwe2k14_{n}.log'), encoding='utf-8', errors='replace'):
            m = re.search(r'review: clip_(start|stop) (\S+) \(frame (\d+)\)', line)
            if m and m.group(2) == name:
                if m.group(1) == 'start':
                    f0 = int(m.group(3))
                else:
                    f1 = int(m.group(3))
        ms = [float(r['frame_ms']) for r in csv.DictReader(open(os.path.join(LOGS, f'frames_{n}.csv')))
              if f0 is not None and f1 is not None and f0 < int(r['frame']) <= f1]
    except OSError:
        return None
    if not ms:
        return None
    # Slow-frame threshold from the arm's own pace (30 fps default entrances vs 60 fps presets / matches):
    # 1.25 x the median frame time, so a steady 30 fps clip does not read as "every frame slow".
    med = sorted(ms)[len(ms) // 2]
    thr = round(1.25 * med, 1)
    return {'frames': len(ms), 'fps': round(len(ms) / (sum(ms) / 1000), 2), 'thr_ms': thr,
            'slow': sum(m > thr for m in ms), 'gt35': sum(m > 35 for m in ms), 'worst_ms': round(max(ms), 1)}


def ingame_still_times(jobdir, clip):
    """In-game clips: {still label: seconds after the clip's first frame}, from the clip's start frame
    (clip_NAME.json), the still's frame (the run log's "screenshot 'LABEL' (frame N)") and the per-frame times in
    frames_NNN.csv. Empty for desktop captures or when anything is missing."""
    import csv
    import re
    try:
        with open(os.path.join(jobdir, 'job.json'), encoding='utf-8') as f:
            job = json.load(f)
        if job.get('capture') != 'ingame' or not job.get('run'):
            return {}
        with open(os.path.join(jobdir, f'clip_{clip}.json'), encoding='utf-8') as f:
            start = int(json.load(f)['start_frame'])
        n = f"{int(job['run']):03d}"
        frames = {}
        for line in open(os.path.join(LOGS, f'wwe2k14_{n}.log'), encoding='utf-8', errors='replace'):
            m = re.search(r"screenshot '(rv_" + re.escape(clip) + r"_t[^']+)' \(frame (\d+)\)", line)
            if m:
                frames[m.group(1)] = int(m.group(2))
        ts = {int(r['frame']): float(r['t_seconds'])
              for r in csv.DictReader(open(os.path.join(LOGS, f'frames_{n}.csv'), encoding='utf-8'))}
        return {lab: ts[f] - ts[start] for lab, f in frames.items() if f in ts and start in ts}
    except (OSError, ValueError, KeyError):
        return {}


def content_check(jobdir, clip):
    """Does the clip show the game? For each native still taken during the clip (still_rv_<clip>_tSSpS.png, S =
    seconds after clip_start) find the best-matching clip frame within -0.8..+0.4 s (the first captured frame is
    ~0.2 s after the log line) and return the best mean abs difference (grey, 320x180). Game: ~0.3-1.3 in batch1;
    a capture of another window (run 186's desktop frame) is > 100. Frames are piped, nothing is written."""
    import glob
    import re
    import numpy as np
    sys.path.insert(0, HERE)
    import make_routes
    # still labels are seconds after the route's anchor; the clip starts make_routes' clip offset after it
    off = {make_routes.ENTRANCE['clip'][0]: make_routes.ENTRANCE['clip'][1],
           make_routes.MATCH['clip'][0]: make_routes.MATCH['clip'][1]}.get(clip, 0.0)
    mp4 = os.path.join(jobdir, f'clip_{clip}.mp4')
    real = ingame_still_times(jobdir, clip)
    res = {}
    for png in sorted(glob.glob(os.path.join(jobdir, f'still_rv_{clip}_t*.png'))):
        m = re.search(r'_t(\d+)p(\d)\.png$', png)
        if not m:
            continue
        t = int(m.group(1)) + int(m.group(2)) / 10 - off
        # In-game clips run in real time: when the game ran slower than its nominal rate (4K preset entrances with
        # capture), a still N frames after clip_start is later than N / fps in the clip. Use the measured time.
        t = real.get(os.path.basename(png)[6:-4], t)
        ref = np.asarray(Image.open(png).convert('L').resize((320, 180), Image.LANCZOS), dtype=np.float32)
        r = subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-ss', f'{max(0.0, t - 0.8):.2f}',
                            '-t', '1.2', '-i', mp4, '-vf', 'scale=320:180:flags=area,format=gray', '-f', 'rawvideo',
                            '-'], capture_output=True)
        buf = np.frombuffer(r.stdout, dtype=np.uint8)
        n = buf.size // (320 * 180)
        if not n:
            continue
        fr = buf[:n * 320 * 180].reshape(n, 180, 320).astype(np.float32)
        res[os.path.basename(png)[6:-4]] = round(float(np.abs(fr - ref).mean(axis=(1, 2)).min()), 2)
    return res


def pts_gaps(mp4, game_fps=None):
    """Capture frames missing from a clip: gaps between frame timestamps over 1.5 frame periods (60 fps capture).
    ddagrab does not report frames it missed (no dup= / drop= in ffmpeg's output), so this is the only measure."""
    r = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_entries', 'frame=pts_time',
                        '-of', 'csv=p=0', mp4], capture_output=True, text=True)
    ts = [float(x) for x in r.stdout.split() if x.strip() and x.strip() != 'N/A']
    if len(ts) < 2:
        return None
    gaps = [b - a for a, b in zip(ts, ts[1:])]
    return {'frames': len(ts), 'span_s': round(ts[-1] - ts[0], 2), 'gaps_over_25ms': sum(g > 0.025 for g in gaps),
            'max_gap_ms': round(1000 * max(gaps), 1)}


def load_jobs(bdir):
    jobs = {}
    rdir = os.path.join(bdir, 'runs')
    if not os.path.isdir(rdir):
        return jobs
    for d in sorted(os.listdir(rdir)):
        p = os.path.join(rdir, d, 'job.json')
        if os.path.exists(p):
            with open(p, encoding='utf-8') as f:
                j = json.load(f)
            j['dir'] = os.path.join(rdir, d)
            jobs[(j['route'], j['arm'])] = j
    return jobs


DEGRADED_MISSED = 0.05  # same threshold as record.py: more missed game frames than this = degraded clip


def degraded(j, clip):
    """'' or why an in-game clip is degraded: it missed more than 5% of the game's frames (no free capture thread,
    encoder backlog, failed captures; run 395's Undertaker 4K match: 199 of 900). From the clip's own counts, so
    jobs recorded before record.py stored capture_degraded are judged the same way."""
    if j.get('capture') != 'ingame':
        return ''
    c = next((c for c in j.get('clips', []) if c.get('name') == clip), {})
    g = c.get('ingame') or {}
    missed = (g.get('dropped_busy') or 0) + (g.get('dropped_backlog') or 0) + (g.get('capture_failed') or 0)
    want = g.get('capture_requests') or g.get('triggers') or 0  # older jsons: one request per game frame
    if want and missed / want > DEGRADED_MISSED:
        return f'missed {missed} of {want} wanted frames, {100.0 * missed / want:.1f}%'
    return ''


def job_usable(j):
    """Only arms that record.py marked capture_ok go into clips and sheets (a crashed run or a wrong-window capture
    would otherwise be published as an arm)."""
    return j.get('capture_ok') is True


def region_for(label, route=None):
    if (route, label) in REGIONS_EXACT:
        return REGIONS_EXACT[(route, label)]
    for k, v in REGIONS.items():
        if label.startswith(k):
            return v
    return None


# ---- batch definitions -------------------------------------------------------------------------------------------
COMMON_NOTE = ('Clips are desktop captures of the presented 1280x720 window (the 1440p/4K presets are downscaled '
               'to 720p for display, so the clips show their supersampled edges, not their resolution). The stills '
               'are the internal render at native resolution: the 100% crops show the resolution difference a 720p '
               'desktop cannot. Arms are separate runs: camera variants and animation can differ between them.')
# In-game clips (record.py --capture ingame, the default since Session 8 phase 2 task t1; src/clip_capture.h).
INGAME_NOTE = ('Clips are recorded inside the game: the guest output at the internal render resolution (1280x720 '
               'default, 2560x1440 and 3840x2160 with the presets; the 4khalf arm at half size), h264_nvenc, in '
               'real time at the clip frame rate given on each arm\'s line (60 fps, 30 fps for the 4k30 arm; the '
               'previous frame is repeated when the game shows a frame for longer, e.g. the 30 fps default '
               'entrances). The side-by-side video scales every arm to 1280x720 at 60 fps and is as long as the '
               'longest arm (shorter arms hold their last frame); each arm\'s own clip at native resolution is '
               'linked under the video. Recording costs the game frames at 4K60 in heavy scenes (the fps on each '
               'line is the game\'s with the capture running). The stills are the native internal render too (100% '
               'crops). Arms are separate runs: camera variants and animation can differ between them.')


def ingame_line(c):
    """One-line summary of an in-game clip's own counts (clip_NAME.json): frames the game rendered that the clip
    lacks (no free capture thread / encoder backlog / failed capture) and repeats."""
    g = c.get('ingame') or {}
    if not g:
        return '; in-game clip: no counts (clip json missing)'
    missed = (g.get('dropped_busy') or 0) + (g.get('dropped_backlog') or 0) + (g.get('capture_failed') or 0)
    trig = g.get('triggers') or 0
    same = g.get('identical_to_previous') or 0
    s = (f"; in-game clip {g.get('width')}x{g.get('height')} at {g.get('fps'):g} fps: {g.get('video_frames')} frames "
         f"({g.get('video_seconds'):.2f} s), game frames {trig}, missed {missed} (busy {g.get('dropped_busy')}, "
         f"backlog {g.get('dropped_backlog')}, failed {g.get('capture_failed')}), same image as the capture before "
         f"{same}, repeated {g.get('repeated')}, skipped {g.get('skipped')}, capture {g.get('capture_ms_avg'):.1f} ms "
         'avg')
    # 'same image' = the capture ran before the GPU had refreshed the output (a frame shown twice, the next one
    # lost) - or a static scene, where it is harmless.
    want = g.get('capture_requests') or trig  # a 30 fps clip of a 60 fps game wants every other frame
    if g.get('capture_requests') is not None:
        s += f", capture requests {want} (skipped before capture {g.get('skipped_before_capture')})"
    if want and missed + same > max(3, 0.02 * want):
        s += (' - CAPTURE MISSED FRAMES the game rendered (missed + same image > 2%; they show as repeats unless the '
              'scene was static): uneven motion in this clip may be the capture, not the game')
    return s

BATCH1 = [
    {'id': 'edges_bryan_entrance', 'group': '1. Edges without MSAA and the crowd up close',
     'title': "Bryan's entrance: stage, ramp and wide crowd (Over the Limit)",
     'route': 'bryan', 'clip': 'e2', 'stills': ['rv_e2_t09p0', 'rv_e2_t14p7', 'rv_e2_t20p0'],
     'arms': ['default', '1440p', '4k'],
     'note': 'Edges of the barricade, ramp, stage truss and lights: are the no-MSAA presets jaggier or shimmering '
             'while the camera pans? Crowd: does it look sharper (1440p/4K) or noisier / flickery? '
             'Default runs the entrance at 30 fps, the presets at 60 (already approved); judge edges, not motion. '
             'Camera variants differ between these runs: the 1440p run (187) got the other variant of this entrance '
             '(close-up with the nameplate at ~16-20 s), the default (198) and 4K (188) runs the same one.'},
    {'id': 'edges_taker_entrance', 'group': '1. Edges without MSAA and the crowd up close',
     'title': "Undertaker's entrance: CH103 crowd stretch (WrestleMania Moment arena)",
     'route': 'taker', 'clip': 'e2', 'stills': ['rv_e2_t09p0', 'rv_e2_t14p7', 'rv_e2_t20p0'],
     'arms': ['default', '1440p', '4k'],
     'note': 'The heaviest crowd shot in the game so far: thousands of crowd members, smoke and stage lights. '
             'Look for crawling / flickering edges in the crowd and stage rigging, and the sharpness of the crowd '
             'in the 100% crops. The 4K run (196) got a different camera from ~9 s to ~15 s (a side-on wide shot); '
             'default (194) and 1440p (195) match.'},
    {'id': 'edges_bryan_match', 'group': '1. Edges without MSAA and the crowd up close',
     'title': 'In the match: medium in-ring shot with the crowd behind (Lesnar vs Bryan)',
     'route': 'bryan', 'clip': 'match', 'stills': ['rv_match_t10p0', 'rv_match_t15p0'],
     'arms': ['default', '1440p', '4k'],
     'note': 'The closest regular match camera these routes reach. Ropes, turnbuckles and the ring apron are the '
             'thin, high-contrast edges where missing MSAA shows first (stair-stepping, shimmer as the camera '
             'moves). Wrestler skin, hair and trunks edges in the crops. The match itself plays out differently in '
             'each run (CPU vs CPU), so positions and camera angles differ.'},
    {'id': 'edges_taker_match', 'group': '1. Edges without MSAA and the crowd up close',
     'title': 'In the match: WrestleMania Moment arena (Lesnar vs Undertaker)',
     'route': 'taker', 'clip': 'match', 'stills': ['rv_match_t10p0', 'rv_match_t15p0'],
     'arms': ['default', '1440p', '4k'],
     'note': 'Same as above in the bigger arena: ropes, barricade and crowd edges. The clip includes a low-angle '
             'close-up through the ropes (long thin diagonal edges).'},
    {'id': 'ringside', 'group': '1. Edges without MSAA and the crowd up close',
     'title': 'Ringside (barricade, announce table, front-row crowd)', 'missing': True,
     'note': "Needs Jarrod's recording: no route reaches a ringside camera yet (a match where a wrestler is thrown "
             'outside). Record one with --wwe2k14_input_record and it can be added to this batch.'},
    {'id': 'caw', 'group': '2. Create a Wrestler and menus (4K preset vs default)',
     'title': 'Main menu, CREATIONS menu and the Create a Wrestler screen',
     'route': 'caw', 'stills': ['rv_menu_main', 'rv_menu_creations', 'rv_menu_superstar_new_edit', 'rv_caw_t02',
                                'rv_caw_t10', 'rv_caw_t33'], 'arms': ['default', '4k'],
     'note': 'Stills only: main menu, CREATIONS, SUPERSTAR > NEW / EDIT, the loading screen, HEAD > TEMPLATES, and '
             'the template model on SELECT A PART TO EDIT (Create a Superstar = Create a Wrestler; nothing saved). '
             'Look at text and icon edges (at 4K the menus render at native 4K), the model (skin, trunks and '
             'silhouette edges) and anything missing, misplaced or blurred at 4K compared with default.'},
    {'id': 'logo', 'group': '3. Create a Logo (pre-checked)',
     'title': 'Create a Logo: grid, editor, stamp categories, SELECT FONT (Lettering)', 'logo': True,
     'note': 'Already pre-checked (Lettering PASS at 4K, run 175; downscaled vs default run 146: MAD 0.8-1.1, '
             '<= 0.11% of pixels off by > 32 = resampling only). Just a glance: text, icons and the canvas grid '
             'at 4K vs default.'},
]


def build(batch, check, arm_map=None):
    bdir = os.path.join(REVIEW, batch)
    all_jobs = load_jobs(bdir)
    jobs = apply_arm_map(all_jobs, arm_map or {})
    checks = {}
    sections = []
    for it in BATCH1:
        sec = {'item': it, 'video': None, 'sheets': [], 'runs': [], 'missing_arms': []}
        if it.get('missing'):
            sections.append(sec)
            continue
        if it.get('logo'):
            for shot in ('logo_grid', 'editor', 'stamp_categories', 'after_lettering'):
                arms = [('Default, run 146', os.path.join(CAPTURES, f'146_{shot}.bmp')),
                        ('4K preset, run 175', os.path.join(CAPTURES, f'175_{shot}.bmp'))]
                if all(os.path.exists(p) for _, p in arms):
                    out = os.path.join(bdir, 'sheets', f'logo_{shot}.png')
                    still_sheet(arms, REGIONS['logo'], out, f'Create a Logo: {shot}  (default run 146 | 4K run 175)')
                    sec['sheets'].append(os.path.relpath(out, bdir))
            sec['runs'] = ['146 (defaults)', '175 (--wwe2k14_preset_4k60=true)']
            sections.append(sec)
            continue
        recorded = [a for a in it['arms'] if (it['route'], a) in jobs]
        have = [a for a in recorded if job_usable(jobs[(it['route'], a)])]
        sec['failed_arms'] = [f"{a} (run {jobs[(it['route'], a)].get('run')}: "
                              f"{(jobs[(it['route'], a)].get('run_list') or ['no run_list line'])[-1].split('->')[-1].strip()})"
                              for a in recorded if a not in have]
        sec['missing_arms'] = [a for a in it['arms'] if a not in recorded]
        gaps_by_arm = {}
        for a in have:
            j = jobs[(it['route'], a)]
            shown = a if j['arm'] == a else f"{a} (shown: the {j['arm']} job, --arm-map)"
            line = (f"{shown}: run {j['run']} ({j['flags'] or 'default settings'}) - "
                    f"{(j['run_list'] or ['?'])[-1].split('->')[-1].strip()}")
            if it.get('clip') and degraded(j, it['clip']):
                line += (f"; DEGRADED CAPTURE ({degraded(j, it['clip'])}): motion in this clip is the capture's, "
                         "not the game's; judge motion on another arm or re-record (record.py --force, or a cheaper "
                         'arm: 4k30 / 4khalf with --arm-map)')
            if j.get('route_sha256'):
                line += f"; route {j['route_sha256'][:10]}"
            st = clip_window_stats(j['run'], it['clip']) if it.get('clip') and j.get('run') else None
            if st:
                line += (f"; game during the clip (capture running): {st['fps']} fps, {st['frames']} frames, "
                         f"{st['slow']} slower than {st['thr_ms']} ms (1.25 x median), worst {st['worst_ms']} ms")
            ingame = j.get('capture') == 'ingame'
            if it.get('clip') and ingame:
                c = next((c for c in j.get('clips', []) if c.get('name') == it['clip']), {})
                line += ingame_line(c)
                p = os.path.join(j['dir'], f"clip_{it['clip']}.mp4")
                if os.path.exists(p):
                    g = c.get('ingame') or {}
                    sec.setdefault('native', []).append(
                        (f"{arm_short(j['arm'])} {g.get('width')}x{g.get('height')} {g.get('fps', 0):g} fps",
                         os.path.relpath(p, bdir)))
            if it.get('clip') and check:
                p = os.path.join(j['dir'], f"clip_{it['clip']}.mp4")
                # In-game clips are constant frame rate by construction (no timestamp gaps): their misses are the
                # game's own counts above. Gaps are measured for desktop captures only.
                g = pts_gaps(p) if os.path.exists(p) and not ingame else None
                if g:
                    line += (f"; capture: {g['frames']} frames over {g['span_s']} s, {g['gaps_over_25ms']} gaps "
                             f"> 25 ms (max {g['max_gap_ms']} ms){{GAPFLAG_{a}}}")
                    gaps_by_arm[a] = g['gaps_over_25ms']
                cc = content_check(j['dir'], it['clip']) if os.path.exists(p) else {}
                if cc:
                    worst = max(cc.values())
                    # A clip slower than the game (4k30: 30 fps of a 60 fps game) holds every other game frame, so a
                    # still's own frame may not be in it and the best match is a neighbour one game frame away
                    # (fixer pass, run 423 rv_e2_t14p7: MAD 7.01 / 7.05 for the frames either side, same moment and
                    # scene by eye). Desktop-capture batch1 and full-rate in-game clips: 0.3-2.3.
                    clip_fps = ((c.get('ingame') or {}).get('fps') or 60) if ingame else 60
                    sub = ingame and clip_fps < 50
                    limit = 10 if sub else 5
                    line += f"; clip vs native stills: best MAD {sorted(cc.values())} "
                    line += ('(shows the game' + (f'; a {clip_fps:g} fps clip lacks every other game frame, so its '
                                                   'best match can be a neighbour of the still: up to ~10 in fast '
                                                   'motion)' if sub and worst > 5 else ')') if worst <= limit else
                             '- NO CLOSE MATCH for a still: an in-game clip records only the game, so this means '
                             'the frames near that still were missed (fast motion; see missed above)' if ingame else
                             '- WRONG CONTENT: the clip does not match the game stills (another window captured?)')
            sec['runs'].append(line)
        # Desktop capture always misses a few slots (batch1: 3-13 gaps > 25 ms per clip in every arm). Flag an arm
        # whose capture missed clearly more than the item's median arm (Undertaker 4K entrance: 21 vs 6), since
        # that can look like stutter the game did not have.
        if gaps_by_arm:
            med = sorted(gaps_by_arm.values())[len(gaps_by_arm) // 2]
            for a, n in gaps_by_arm.items():
                flag = (f' - CAPTURE MISSED MORE FRAMES than the other arms ({n} gaps vs median {med}): uneven '
                        'motion in this clip may be the capture, not the game; judge motion on another arm or '
                        're-record') if len(gaps_by_arm) >= 2 and n > 1.5 * med + 3 else ''
                sec['runs'] = [r.replace(f'{{GAPFLAG_{a}}}', flag) for r in sec['runs']]
        hashes ={jobs[(it['route'], a)].get('route_sha256') for a in have}
        unknown = [str(jobs[(it['route'], a)].get('run')) for a in have
                   if not jobs[(it['route'], a)].get('route_sha256')]
        if len(hashes - {None}) > 1:
            sec['runs'].append('WARNING: the arms ran different route revisions (route_sha256 differs).')
        if unknown:
            sec['runs'].append(f"Route revision not recorded for run(s) {', '.join(unknown)} (recorded before "
                               'record.py stored route_sha256).'
                               + (' Run 193 (CAW default) ran the route before its two wait_file syncs were added.'
                                  if '193' in unknown else ''))
        if it.get('clip') and len(have) >= 2:
            cl = []
            for a in have:
                p = os.path.join(jobs[(it['route'], a)]['dir'], f"clip_{it['clip']}.mp4")
                if os.path.exists(p):
                    ja = jobs[(it['route'], a)]
                    deg = ' DEGRADED' if degraded(ja, it['clip']) else ''
                    cl.append((f"{arm_short(ja['arm'])} (run {ja['run']}){deg}", p))
                    if check:
                        checks[os.path.relpath(p, bdir)] = sample_frames(p, os.path.join(bdir, 'checks'))
            if len(cl) >= 2:
                out = os.path.join(bdir, 'clips', f"{it['id']}.mp4")
                lens = side_by_side(cl, out)
                sec['video'] = os.path.relpath(out, bdir)
                padded = [f'{lab} {s:.2f} s (+{p:.2f} s held)' for lab, s, p in lens if p > 0.25]
                if padded:
                    sec['runs'].append('Side-by-side video: the arms are not cut to the shortest; shorter clips hold '
                                       'their last frame at the end. Clip lengths differ because each clip runs in '
                                       'real time and the game ran slower in some arms: '
                                       + '; '.join(f'{lab} {s:.2f} s' for lab, s, _ in lens)
                                       + '. So late in the video the arms are a little apart in game time.')
                if check:
                    checks[sec['video']] = sample_frames(out, os.path.join(bdir, 'checks'), n=3)
        if len(have) >= 2:
            j0 = jobs[(it['route'], have[0])]
            labels = it['stills'] if isinstance(it['stills'], list) else \
                [s['label'] for s in j0['stills'] if s['label'].startswith(it['stills'])]
            for lab in labels:
                arms = []
                for a in have:
                    p = os.path.join(jobs[(it['route'], a)]['dir'], f'still_{lab}.png')
                    if os.path.exists(p):
                        arms.append((f"{arm_short(jobs[(it['route'], a)]['arm'])} "
                                     f"(run {jobs[(it['route'], a)]['run']})", p))
                if len(arms) >= 2:
                    out = os.path.join(bdir, 'sheets', f"{it['id']}_{lab}.png")
                    still_sheet(arms, region_for(lab, it['route']), out, f"{it['title']}: {lab}")
                    sec['sheets'].append(os.path.relpath(out, bdir))
        sections.append(sec)
    if check:
        with open(os.path.join(bdir, 'checks.json'), 'w', encoding='utf-8') as f:
            json.dump(checks, f, indent=1)
    write_html(bdir, batch, sections, all_jobs)
    return sections, checks


def url(path):
    """A relative path as a URL (forward slashes)."""
    return path.replace(os.sep, '/')


def write_html(bdir, batch, sections, jobs):
    e = html.escape
    cap = None
    allc = [(j, c) for j in jobs.values() for c in j.get('clips', []) if j.get('capture') != 'ingame']
    gamec = [(j, c) for j in jobs.values() for c in j.get('clips', []) if j.get('capture') == 'ingame']
    if allc:
        lat = max(c.get('start_latency_s') or 0 for _, c in allc)
        cpu = max((c.get('ffmpeg_cpu_s') or 0) / max(c['probe'].get('duration') or 1, 1) for _, c in allc)
        cap = (allc[0][0].get('capture'), round(lat, 3), f'{100 * cpu:.1f}%', len(allc))
    out = ['<!doctype html><html lang="en"><head><meta charset="utf-8">',
           '<meta name="viewport" content="width=device-width, initial-scale=1">',
           f'<title>Review {e(batch)}</title><style>',
           ':root{--bg:#111;--fg:#eee;--mut:#aaa;--acc:#ffd84a;--card:#1c1c1c}',
           'body{background:var(--bg);color:var(--fg);font:15px/1.45 system-ui,Segoe UI,sans-serif;margin:0;'
           'padding:16px;max-width:1500px}',
           'h1{font-size:22px}h2{font-size:18px;margin-top:32px;color:var(--acc)}h3{font-size:16px;margin:0 0 6px}',
           '.card{background:var(--card);border-radius:8px;padding:14px;margin:14px 0}',
           '.note{margin:6px 0}.verdict{border-top:1px dashed #555;padding-top:8px;margin-top:10px;color:var(--mut)}',
           'video,img{max-width:100%;height:auto;display:block;margin:8px 0;border-radius:4px}',
           '.mut{color:var(--mut);font-size:13px}a{color:#8cf}ul{margin:4px 0}</style></head><body>',
           f'<h1>Visual review queue: {e(batch)}</h1>',
           '<p>For each item: watch / look, then write one word on the Verdict line (ok / worse / better / bug) '
           'and a few words if something is off. Already approved and not queued again: titantrons, outfits, '
           'entrance music and sound-cue sync, smoke (60 fps entrances).</p>',
           f'<p class="mut">{e(INGAME_NOTE if gamec and not allc else COMMON_NOTE)}</p>']
    if gamec:
        g = [c.get('ingame') or {} for _, c in gamec]
        missed = sum((x.get('dropped_busy') or 0) + (x.get('dropped_backlog') or 0) + (x.get('capture_failed') or 0)
                     for x in g)
        trig = sum(x.get('triggers') or 0 for x in g)
        out.append(f'<p class="mut">Capture (in-game, src/clip_capture.h): {len(gamec)} clips recorded by the game '
                   'itself from the presented guest output (works with the display asleep). Over all of them the '
                   f'capture missed {missed} of {trig} game frames (each per-run line has its clip\'s counts; a clip '
                   'that missed more than 2% is flagged). The game\'s fps during each clip (capture running) is on '
                   'the same line.' + (' Some arms are still older desktop captures (720p, see their lines).'
                                       if allc else '') + '</p>')
    if cap:
        out.append(f'<p class="mut">Capture: ffmpeg {e(str(cap[0]))} (Desktop Duplication) to h264_nvenc at 60 fps, '
                   'the game window put in front for the run (no focus change); over all '
                   f'{cap[3]} recorded clips ffmpeg was started at most {cap[1]} s after the route\'s clip_start log '
                   f'line and used at most {cap[2]} of one CPU core (spawn time; the first captured frame is about '
                   '0.2 s after the log line, the same for every arm). The per-run lines under each item give the '
                   "game's fps during the clip (capture running) and, with --check, capture frames missed (gaps); "
                   'a clip whose capture missed frames the game rendered is flagged there. Clip videos are '
                   'several 1280x720 views wide: open one on its own (link) for full size.</p>')
    group = None
    for s in sections:
        it = s['item']
        if it['group'] != group:
            group = it['group']
            out.append(f'<h2>{e(group)}</h2>')
        out.append('<div class="card">')
        out.append(f"<h3>{e(it['title'])}</h3>")
        out.append(f"<p class=\"note\"><b>Look at:</b> {e(it['note'])}</p>")
        if s['video']:
            v = e(url(s['video']))
            out.append(f'<video controls loop muted playsinline preload="metadata" src="{v}"></video>')
            native = ''.join(f' | <a href="{e(url(p))}">{e(lab)}</a>' for lab, p in s.get('native', []))
            out.append(f'<p class="mut"><a href="{v}">open the clip on its own</a>'
                       + (f'{native} (each arm at native resolution)' if native else '') + '</p>')
        for sh in s['sheets']:
            out.append(f'<a href="{e(url(sh))}"><img loading="lazy" src="{e(url(sh))}" '
                       f'alt="{e(os.path.basename(sh))}"></a>')
        if s['runs']:
            out.append('<ul class="mut">' + ''.join(f'<li>{e(r)}</li>' for r in s['runs']) + '</ul>')
        if s.get('failed_arms'):
            out.append('<p style="color:#f66">FAILED, not compared (run did not exit cleanly or capture not OK): '
                       f"{e('; '.join(s['failed_arms']))}</p>")
        if s['missing_arms']:
            out.append(f"<p class=\"mut\">Not recorded (yet): {e(', '.join(s['missing_arms']))}</p>")
        if not it.get('missing') and not s['video'] and not s['sheets']:
            out.append('<p class="mut">No recordings for this item yet.</p>')
        out.append('<p class="verdict">Verdict: ____________________________________________</p></div>')
    cj = os.path.join(bdir, 'checks.json')
    if os.path.exists(cj):
        with open(cj, encoding='utf-8') as f:
            checks = json.load(f)
        out.append('<h2>Checks (automatic)</h2><ul class="mut">')
        for k, v in sorted(checks.items()):
            p = v['probe']
            out.append(f"<li>{e(url(k))}: {p['w']}x{p['h']}, {p['frames']} frames, {p['duration']} s; "
                       f"black {v['black']}, frozen {v['frozen']}; mean luma of the sampled frames "
                       f"{[x.get('mean_luma') for x in v['samples']]}</li>")
        out.append('</ul>')
    out.append('</body></html>')
    with open(os.path.join(bdir, 'index.html'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(out))


def verify_links(bdir):
    """Every src/href in index.html must be relative and exist on disk."""
    import re
    page = open(os.path.join(bdir, 'index.html'), encoding='utf-8').read()
    refs = re.findall(r'(?:src|href)="([^"]+)"', page)
    bad = [r for r in refs if re.match(r'^[a-zA-Z]+:|^/', r) or not os.path.exists(os.path.join(bdir, html.unescape(r)))]
    return len(refs), bad


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--batch', default='batch1')
    ap.add_argument('--check', action='store_true')
    # Default 4k=4k30 (fixer pass, measured): a 4K60 in-game clip slowed the heavy 4K entrances (Undertaker 57.89 fps,
    # run 395; Bryan 54.82, run 396), a 30 fps clip did not (59.94 fps, run 416), so where a 4k30 job exists it is
    # what the page shows as the 4K arm. --arm-map '' shows the 4k (60 fps clip) jobs.
    ap.add_argument('--arm-map', default='4k=4k30', metavar='ITEM_ARM=JOB_ARM,...',
                    help='show a recorded job of another arm in place of an item arm (default 4k=4k30: record.py '
                    '--jobs taker:4k30 first); routes without that job keep the item arm\'s own job; \'\' = off')
    a = ap.parse_args()
    arm_map = {}
    for pair in filter(None, a.arm_map.split(',')):
        if '=' not in pair:
            ap.error(f'--arm-map: {pair!r} is not ITEM_ARM=JOB_ARM')
        k, v = (x.strip() for x in pair.split('=', 1))
        arm_map[k] = v
    # Heavy work (CPU decode, NVENC encodes, 4K PNG sheets): take the run lock so it never overlaps a game run
    # (compose overlapped the regress perf runs 201/202 and inflated their slow-frame counts).
    sys.path.insert(0, os.path.join(REPO, 'tools', 'harness'))
    from runlock import ensure_lock
    ensure_lock('review compose ' + a.batch)
    sections, checks = build(a.batch, a.check, arm_map)
    n, bad = verify_links(os.path.join(REVIEW, a.batch))
    print(f'index.html: {n} links, {len(bad)} broken or absolute: {bad}')
    for s in sections:
        print(f"{s['item']['id']}: video {s['video']}, sheets {len(s['sheets'])}, missing {s['missing_arms']}")
    for k, v in checks.items():
        print(f"check {k}: {v['probe']} black={v['black']} frozen={v['frozen']} "
              f"luma={[x.get('mean_luma') for x in v['samples']]} diff={[x.get('diff_prev') for x in v['samples']]}")


if __name__ == '__main__':
    sys.exit(main())
