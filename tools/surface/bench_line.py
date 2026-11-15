#!/usr/bin/env python3
"""S102 lane M3: one line per full Surface bench.

  py -I tools\\surface\\bench_line.py RUN_DIR [--ledger CSV --when 'YYYY-MM-DD HH:MM'] [--run N] [--allow-no-heat]

RUN_DIR is a folder of bench steps (frames_bryan, frames_brock, frames_match: the shape tools\\surface\\bench.ps1 leaves, e.g.
out\\s101\\surface_rebench\\y) or one step folder. The newest run (highest NNN) of each step is read. Prints ONE line:

  bench y (runs ..) | arm wwe2k14_fixed_seed=1 | settings settings_native720.json | tier STANDARD | res 720p (draw scale 1) | heat ... |
  match [frames 5058..5658]: shown 28.3 fps, 1% low 8.0 | gpu 11.2 render - swap 20.0 ms | bryan ...

Windows are the ROUTE's own marks, `[wwe2k14][input] line N: scene=NAME ... (frame F)` (power_read.py has the regex): match = scene=match .. scene=end;
entrances = scene=step15 .. scene=end (and tron_NNN .. scene=end, printed as a second, labelled window "tron"). A log without scene=end (or without
the start mark) is REFUSED: the CSV's last row is the quit frame (1.6-2.5 s) and never an end. The window is printed.
Numbers: shown fps = presents / sum(frame_ms) of the window's swaps (the `presented` column is a COUNT of presents since the last swap, not a flag);
1% low from present_gap_ms; gpu = mean gpu_ms; swap = mean frame_ms; render = the render thread's column when the frames CSV has one
(render_thread_ms / render_cpu_ms / rdr_render_ms), else the threads.csv thread whose name has 'render' (cpu_ms per swap over the window), else
"not measured" (the old gpu_thread_cpu_ms is the GUEST GPU command thread, not ours, and is not used).

REFUSALS (exit 2, one REFUSED line on stderr, nothing on stdout): no settings-file / quality-tier line; steps whose command-line flags differ (the
arm: every `source: command line` cvar except paths, scenario and wwe2k14_launch*; the arm is printed); no scene marks / no scene=end; heat: a
power.csv that is missing, empty, stale (heat_rules.check_fresh: its mtime against the log's), or whose clock-of-day span does not overlap the
log's (the step folders' power.csv of 2026-11-07 beside the 2026-11-13 logs), or that has no sample inside the window.
HEAT (CLAUDE.md: a bench states settings file, tier, resolution and heat): heat_verdict.txt of the step (S0-2), then window values from power.csv:
cpu_perf % mean, perf_limit median / min, perf_limit_flags set, package W mean (rapl_pkg is milliwatts). cpu_freq_mhz / cpu_nominal_mhz are
fixed-rate counters on the Surface and are never printed as heat.
--allow-no-heat prints the line anyway with 'heat NOT RECORDED (reason)' and the prefix 'NOT A BENCH NUMBER' (the offline checks use it on old folders).
--ledger CSV appends one 'bench' row per scene/side to the budget ledger (docs\\renderer\\BUDGET_LEDGER.csv).

S102 H3 (the swap-thread split; docs\\renderer\\FRAMES_CSV_SPLIT.md, src\\frame_split_math.h): a run with --wwe2k14_frame_split=true (bench.ps1 -FrameSplit) has seven more
columns at the END of frames_NNN.csv, ms per guest frame on the game's swap thread, in this order:
  fs_reader_draw_ms     our reader's per-draw work (reader.h g_times.draw: shaders, targets, state, texture slots, banks, geometry all nested inside it, NOT added again)
  fs_reader_present_ms  our reader's present work (g_times.present; the worker join sits inside it on the live path)
  fs_scene_end_ms       the scene-live EndFrame (rdr_scene::live::EndFrame, once per present)
  fs_replay_ms          the walk of sub_82BB5BE8 (loop + interpreter + our untimed hook work) minus the draw / present / scene end / fence wait parts MEASURED inside it
  fs_fence_wait_ms      the wait on the GPU fence (the gwait_b / gwait_e pairs of gpu_timeline.h), wall time
  fs_barrier_wait_ms    the wait on the update thread's frame barrier (sub_82C3BAE0, the toml hooks of barrier_probe.h), wall time
  fs_rest_ms            thread CPU (game_cpu_ms) minus draw, present, scene end and replay: CPU minus wall-time parts, can be negative; an empty cell when the CPU is unknown
--split prints, per window, a second line with each column's mean and the mean over the 1 %-low frames (the frames whose present gap is at or above the window's
p99 gap): `split [bryan frames A..B]: draw 8.1/9.3 ...` = mean/p99-frame mean. A csv without the columns prints 'split: not measured'. The header is checked: the csv's
fs_ columns must sit at the END, in this order, with these names (a prefix of the seven); every other column is read by name as before. A split column in front
of another column, out of order, or an unknown fs_ name is REFUSED.
"""
import argparse, csv, re, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import heat_rules
import burst_rules   # S102 B1
import wer_report   # S102 TS1

RES_NAMES = {0: '720p', 1: '1440p', 2: '4K'}   # src\launcher\launcher_core.h:153 (0 720p, 1 1440p, 2 4K)
STEPS = (('bryan', 'frames_bryan'), ('brock', 'frames_brock'), ('match', 'frames_match'), ('bryan_heavy', 'frames_bryan_heavy'))   # S102 M6: frames_bryan_heavy = tools\input_scripts\scenesryan_heavy.txt, window scene=heavy .. scene=end
STAMP = re.compile(r'^\[\d{4}-\d\d-\d\d (\d\d):(\d\d):(\d\d\.\d+)\]')
MARK = re.compile(r'\[wwe2k14\]\[input\] line \d+: scene=([A-Za-z0-9_]+)\b[^\n]*?\(frame (\d+)\)\s*$')
SET_RE = re.compile(r'settings file (\S+): upscale (\d+), resolution (\d+), frame rate (\d+), renderer (\d+)(?: -> resolution_scale=(\d+))?')
TIER_RE = re.compile(r'quality tier: (\w+)')
CMD_RE = re.compile(r"modified: (\w+) = '([^']*)' \(source: command line\)")
ARM_SKIP = re.compile(r'^(wwe2k14_launch\w*|game_data_root|user_data_root|cache_root|wwe2k14_replay_dir|wwe2k14_scenario_dir|wwe2k14_input_script|'
                      r'wwe2k14_settings_file|wwe2k14_use_launcher_settings|wwe2k14_timeline)$')
CSV_ORDER = ('frame,t_seconds,frame_ms,game_cpu_ms,gpu_thread_cpu_ms,gpu_ms,draw_rows,gpu_mhz,textures,texture_mb,warm_mb,gpu_wait_ms,gpu_wait_main,scene,presented,'
             'present_gap_ms,rdr_submit_ms,gpu_work_ms,render_thread_ms,acquire_ms,present_call_ms,record_thread_ms,device_lost,present_gap_max_ms,'   # S102 R21-B record_thread_ms; S102 D1 device_lost (1 from a lost Vulkan device on)
             'fs_reader_draw_ms,fs_reader_present_ms,fs_scene_end_ms,fs_replay_ms,fs_fence_wait_ms,fs_barrier_wait_ms,fs_rest_ms').split(',')   # S102 H3: src\\frame_stats.h's header, in order
SPLIT_COLS = tuple(c for c in CSV_ORDER if c.startswith('fs_'))
RENDER_COLS = ('render_thread_ms',)   # S102 S0-3: the live render THREAD's CPU ms a guest frame (bind + parse + Render; thread CPU, waits not in it). NOT rdr_submit_ms (Renderer::Render alone).


class Refused(Exception):
    pass


def clock(h, m, s):
    return int(h) * 3600 + int(m) * 60 + float(s)


def read_log(path):
    """-> dict(marks=[(clock_s, name, frame)], settings, tier, flags={name: value}, t0, t1)"""
    d = {'marks': [], 'settings': None, 'tier': None, 'flags': {}, 't0': None, 't1': None}
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            st = STAMP.match(line)
            if st:
                t = clock(*st.groups())
                if d['t0'] is None:
                    d['t0'] = t
                d['t1'] = t
            if d['settings'] is None:
                m = SET_RE.search(line)
                if m:
                    d['settings'] = m
            if d['tier'] is None:
                m = TIER_RE.search(line)
                if m:
                    d['tier'] = m.group(1)
            m = CMD_RE.search(line)
            if m:
                d['flags'][m.group(1)] = m.group(2)
            m = MARK.search(line)
            if m and st:
                d['marks'].append((clock(*st.groups()), m.group(1), int(m.group(2))))
    return d


def windows(kind, marks, where):
    """-> [(label, frame0, frame1, clock0, clock1)]. Refuses when the start or scene=end mark is missing."""
    ends = [m for m in marks if m[1] == 'end']
    if not ends:
        raise Refused(f'{where}: no scene=end mark in the log (the route did not finish; the CSV\'s last row is the quit frame, never an end)')
    want = [('match', 'match')] if kind == 'match' else [('heavy', 'heavy')] if kind == 'heavy' else [('step15', 'step15')]   # S102 M6: kind 'heavy' = the bryan_heavy route's own mark
    out = []
    for label, name in want:
        st = [m for m in marks if m[1] == name]
        if not st:
            raise Refused(f'{where}: no scene={name} mark in the log, cannot place the {kind} window')
        e = next((x for x in ends if x[2] > st[0][2]), None)
        if e is None:
            raise Refused(f'{where}: no scene=end after scene={name}')
        out.append((label if kind != 'match' else 'match', st[0][2], e[2], st[0][0], e[0]))
    if kind not in ('match', 'heavy'):   # S102 M6: the heavy route has no second (tron) window
        tr = [m for m in marks if m[1].startswith('tron')]
        if tr:
            e = next((x for x in ends if x[2] > tr[0][2]), None)
            if e:
                out.append(('tron', tr[0][2], e[2], tr[0][0], e[0]))
    return out


def fnum(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def check_header(fields, where):
    """S102 H3 (F5): only the fs_ columns are ordered. They sit at the END of the header, in SPLIT_COLS order, with known names (a prefix of it: a csv may stop early); every
    other column is read by name as before, so an older or newer csv with other base columns is read, not refused. A split column in front of another column, out of
    order, or under an unknown fs_ name is REFUSED."""
    fields = [x.strip() for x in (fields or [])]
    fs = [i for i, x in enumerate(fields) if x.startswith('fs_')]
    if not fs:
        return
    names = [fields[i] for i in fs]
    if fs != list(range(len(fields) - len(fs), len(fields))):
        raise Refused(f'{where}: frames csv has a split column (fs_) before a non-split column (column {fs[0] + 1} {fields[fs[0]]!r}): the fs_ columns go at the END '
                      f'(bench_line.py SPLIT_COLS; src\\frame_stats.h)')
    if names != list(SPLIT_COLS[:len(names)]):
        bad = next((i for i, (x, y) in enumerate(zip(names, SPLIT_COLS)) if x != y), min(len(names), len(SPLIT_COLS)))
        raise Refused(f'{where}: frames csv split column {bad + 1} is {names[bad] if bad < len(names) else "(more than known)"!r}, expected {SPLIT_COLS[bad] if bad < len(SPLIT_COLS) else "(end)"!r}: '
                      f'the fs_ columns come in this order: {",".join(SPLIT_COLS)}')


def device_lost_row(rows):
    """S102 D1: the first frames-csv row whose device_lost cell is 1 (the renderer lost its Vulkan device there), or None (never lost, or a csv from before the column)."""
    return next((r for r in rows if (r.get('device_lost') or '').strip() == '1'), None)


def read_frames(csvp):
    with open(csvp, newline='', encoding='utf-8', errors='replace') as f:
        rd = csv.DictReader(f)
        rd.fieldnames   # (reads the header)
        check_header(rd.fieldnames, Path(csvp).name)
        return [r for r in rd if r.get('frame_ms') and r.get('frame')]


def window_numbers(rows, f0, f1, thread_ms=None):
    """Numbers over the swaps with f0 <= frame <= f1. Rows after f1 (the quit frame) are never read."""
    win = [r for r in rows if f0 <= int(r['frame']) <= f1]
    if not win:
        raise Refused(f'frames CSV has no frames in window {f0}..{f1}')
    ms = [float(r['frame_ms']) for r in win]

    def mean_of(col):
        v = [fnum(r.get(col)) for r in win]
        v = [x for x in v if x is not None]
        return sum(v) / len(v) if v else None
    out = {'frames': len(win), 'gpu': mean_of('gpu_ms'), 'swap': sum(ms) / len(ms), 'render': None, 'render_src': 'not measured'}
    out['record'] = mean_of('record_thread_ms') if 'record_thread_ms' in win[0] else None   # S102 R21-B (review F2): rdr_record's CPU (wwe2k14_rdr_stage_split); 'render' stays the bind stage
    for c in RENDER_COLS:
        if c in win[0]:
            out['render'], out['render_src'] = mean_of(c), c
            break
    if out['render'] is None and thread_ms is not None:
        out['render'], out['render_src'] = thread_ms / len(win), 'threads.csv'
    pres = [(int(r['presented']), fnum(r.get('present_gap_ms'))) for r in win if r.get('presented') not in (None, '')]
    if pres:
        n = sum(c for c, _ in pres)
        gaps = sorted(g for c, g in pres if c and g)
        out['fps'] = 1000.0 * n / sum(ms)
        out['low1'] = 1000.0 / gaps[min(len(gaps) - 1, int(0.99 * len(gaps)))] if gaps else None
        out['basis'] = 'shown'
        # S102 R25-C2: the second figure, from present_gap_max_ms (the LARGEST gap of a row, so a 2-present row's long gap is not hidden behind its last one). Headline low1 keeps its
        # rule (the last gap of the row, comparable with every earlier bench); a csv without the column (every csv before R25-C2) has no second figure: low1_max None, same line as before.
        out['low1_max'] = None
        if win and 'present_gap_max_ms' in win[0]:
            mg = sorted(g for g in (fnum(r.get('present_gap_max_ms')) for r in win if r.get('presented') not in (None, '', '0')) if g)
            out['low1_max'] = 1000.0 / mg[min(len(mg) - 1, int(0.99 * len(mg)))] if mg else None
    else:
        s = sorted(ms)
        out['fps'] = 1000.0 * len(ms) / sum(ms)
        out['low1'] = 1000.0 / s[min(len(s) - 1, int(0.99 * len(s)))]
        out['basis'] = 'swaps'
    # S102 H3: the swap-thread split, mean over the window and mean over the 1 %-low frames (present gap at or above the window's p99 gap; the swap interval when the csv has no gaps).
    out['split'] = None
    if all(c in win[0] for c in SPLIT_COLS):
        if pres:
            gv = sorted(g for c, g in pres if c and g)
            cut = gv[min(len(gv) - 1, int(0.99 * len(gv)))] if gv else None
            tail = [r for r in win if cut is not None and fnum(r.get('present_gap_ms')) is not None and fnum(r['present_gap_ms']) >= cut]
        else:
            s_ms = sorted(ms)
            cut = s_ms[min(len(s_ms) - 1, int(0.99 * len(s_ms)))]
            tail = [r for r in win if float(r['frame_ms']) >= cut]

        def mean_in(rs, col):
            v = [x for x in (fnum(r.get(col)) for r in rs) if x is not None]
            return sum(v) / len(v) if v else None
        out['split'] = {c: (mean_in(win, c), mean_in(tail, c)) for c in SPLIT_COLS}
        out['split_tail_n'] = len(tail)
    return out


def render_thread_ms(threads_csv, c0, c1):
    """Total cpu_ms of threads named *render* inside the clock window; None when there is no such thread."""
    p = Path(threads_csv)
    if not p.is_file():
        return None
    tot, seen = 0.0, False
    with open(p, newline='', encoding='utf-8', errors='replace') as f:
        for r in csv.DictReader(f):
            if 'render' in (r.get('name') or '').lower() and r.get('time'):
                try:
                    t = clock(*r['time'].split(':'))
                    v = float(r['cpu_ms'])
                except (ValueError, TypeError):
                    continue
                if c0 <= t <= c1:
                    tot += v
                    seen = True
    return tot if seen else None


def heat(step_dir, log, c0, c1, n):
    """-> text. Raises Refused for a stale or non-overlapping power.csv."""
    step_dir = Path(step_dir)
    pc = step_dir / 'power.csv'
    logp = step_dir / f'wwe2k14_{n}.log'
    try:
        heat_rules.check_fresh(str(pc), str(logp))
    except heat_rules.Refused as e:
        raise Refused(f'{step_dir.name}: heat: {e}')
    with open(pc, newline='', encoding='utf-8', errors='replace') as f:
        rows = [r for r in csv.DictReader(f) if r.get('time') and r['time'] != 'time']
    if not rows:
        raise Refused(f'{step_dir.name}: power.csv has no samples, so no heat record')
    ts = []
    for r in rows:
        try:
            ts.append(clock(*r['time'].split(':')))
        except (ValueError, TypeError):
            raise Refused(f'{step_dir.name}: power.csv time {r["time"]!r} is not hh:mm:ss')
    if log['t0'] is None or ts[-1] < log['t0'] - 60 or ts[0] > log['t1'] + 60:
        raise Refused(f'{step_dir.name}: power.csv covers clock {rows[0]["time"][:8]}..{rows[-1]["time"][:8]}, the run log '
                      f'{int(log["t0"] // 3600):02d}:{int(log["t0"] % 3600 // 60):02d}..{int(log["t1"] // 3600):02d}:{int(log["t1"] % 3600 // 60):02d}: '
                      f'another run\'s file (no overlap)')
    win = [r for t, r in zip(ts, rows) if c0 <= t <= c1]
    if not win:
        raise Refused(f'{step_dir.name}: no power.csv sample inside the window')
    col = lambda k: [x for x in (fnum(r.get(k)) for r in win) if x is not None]
    parts = []
    perf, lim, fl, pk = col('cpu_perf'), col('perf_limit'), col('perf_limit_flags'), col('rapl_pkg')
    if not (perf or lim or fl):
        raise Refused(f'{step_dir.name}: power.csv has no cpu_perf / perf_limit column in the window')
    if perf:
        parts.append(f'perf {sum(perf) / len(perf):.0f} %')
    if lim:
        s = sorted(lim)
        parts.append(f'limit median {s[len(s) // 2]:.0f} min {s[0]:.0f} %')
    if fl:
        parts.append('flags {' + ','.join(f'{x:g}' for x in sorted(set(fl))) + '}')
    if pk:
        parts.append(f'pkg {sum(pk) / len(pk) / 1000:.1f} W')
    v = step_dir / 'heat_verdict.txt'
    verdict = ' '.join(v.read_text(encoding='utf-8', errors='replace').split()) if v.is_file() else 'no heat_verdict.txt'
    # S102 B1: the burst-aware thermal state (burst_rules.py, R28): the window and the 5 s before it, from power.csv.
    samples = []
    for t, r in zip(ts, rows):
        p, w = fnum(r.get('cpu_perf')), fnum(r.get('rapl_pkg'))
        if p is not None and w is not None:
            samples.append((t, p, w / 1000.0))
    th = burst_rules.classify(samples, c0, c1)
    return f'heat [{verdict}] ' + ', '.join(parts) + f' ({len(win)} s), ' + burst_rules.text(th), th


def newest_run(step_dir):
    best = None
    for p in Path(step_dir).glob('frames_*.csv'):
        m = re.fullmatch(r'frames_(\d+)\.csv', p.name)
        if m and (best is None or int(m.group(1)) > best):
            best = int(m.group(1))
    return best


def arm_of(flags):
    return {k: v for k, v in flags.items() if not ARM_SKIP.match(k)}


def read_step(label, step_dir, run=None, allow_no_heat=False):
    step_dir = Path(step_dir)
    n = run if run is not None else newest_run(step_dir)
    if n is None:
        raise Refused(f'{step_dir}: no frames_NNN.csv')
    csvp, logp = step_dir / f'frames_{n}.csv', step_dir / f'wwe2k14_{n}.log'
    if not csvp.is_file() or not logp.is_file():
        raise Refused(f'{step_dir.name}: run {n} lacks frames_{n}.csv or wwe2k14_{n}.log')
    lg = read_log(logp)
    where = f'{step_dir.name} run {n}'
    rows = read_frames(csvp)
    # S102 D1: the frames csv's device_lost column (1 from the first frame the renderer saw its Vulkan device lost): name the frame, before the log's PROBLEM line below.
    lost = device_lost_row(rows)
    if lost is not None:
        raise Refused(f'{where}: the Vulkan device was LOST at guest frame {lost["frame"]} (t={lost.get("t_seconds", "?")} s; frames csv device_lost): nothing was drawn from there, not a valid number')
    # S102 main (2026-11-15 03:1x): a run whose renderer reported a PROBLEM is not a number. A lost Vulkan device (frame_submit_failed:
    # VkResult -4) left the GPU doing nothing while presents still counted, so "shown fps" read ~60 in 6 of 27 Brock runs (and once in
    # Bryan): bar1's "Brock 58.0 past the bar" and ab17's verdicts were such runs. Refuse the step; the reason names the first problem line.
    with open(logp, encoding='utf-8', errors='replace') as fh:
        for line in fh:
            if '[rdr][PROBLEM]' in line:
                raise Refused(f'{where}: the renderer reported a problem, not a valid number: {line.strip()[-160:]}')
    st = lg['settings']
    if st is None:
        raise Refused(f'{where}: no settings-file line in the log (the bench must state its settings file)')
    if lg['tier'] is None:
        raise Refused(f'{where}: no quality-tier line in the log')
    res = RES_NAMES.get(int(st.group(3)))
    if res is None:
        raise Refused(f'{where}: resolution index {st.group(3)} is not a known resolution')
    wins = windows('match' if label == 'match' else 'heavy' if label == 'bryan_heavy' else 'entrance', lg['marks'], where)
    out = []
    for wl, f0, f1, c0, c1 in wins:
        tms = render_thread_ms(step_dir / 'threads.csv', c0, c1)
        nums = window_numbers(rows, f0, f1, tms)
        try:
            h, th = heat(step_dir, lg, c0, c1, n)
        except Refused:
            if not allow_no_heat:
                raise
            h, th = 'heat NOT RECORDED (' + str(sys.exc_info()[1])[:160] + ')', None
        out.append({'label': label if wl in ('match', 'step15', 'heavy') else f'{label}/{wl}', 'run': n, 'win': (f0, f1), 'n': nums, 'heat': h, 'thermal': th})
    return {'label': label, 'run': n, 'settings': Path(st.group(1).replace('\\', '/')).name, 'tier': lg['tier'], 'res': res,
            'scale': st.group(6) or 'none', 'arm': arm_of(lg['flags']), 'windows': out, 'where': where}   # S102 main: profiles without resolution_scale (auto) print none, not a crash


def fmt(v, spec='{:.1f}'):
    return '-' if v is None else spec.format(v)


def line_for(run_dir, run=None, allow_no_heat=False):
    d = Path(run_dir)
    if not d.is_dir():
        raise Refused(f'{d}: not a folder')
    if list(d.glob('frames_*.csv')):
        label = next((k for k, name in STEPS if name == d.name), 'match')
        steps = [read_step(label, d, run, allow_no_heat)]
        name = d.parent.name
    else:
        steps = [read_step(k, d / name, run, allow_no_heat) for k, name in STEPS if (d / name).is_dir()]
        name = d.name
        if not steps:
            raise Refused(f'{d}: no frames_bryan / frames_brock / frames_match folder')
    for s in steps[1:]:
        if s['arm'] != steps[0]['arm']:
            raise Refused(f"steps ran with different flags: {steps[0]['where']} {steps[0]['arm']} vs {s['where']} {s['arm']} (one line per arm)")
    uniq = lambda key: '+'.join(dict.fromkeys(s[key] for s in steps))
    arm = ' '.join(f'{k}={v}' for k, v in sorted(steps[0]['arm'].items())) or 'default'
    runs = '/'.join(str(s['run']) for s in steps)
    heats = [w['heat'] for s in steps for w in s['windows']]
    bad = [h for h in heats if h.startswith('heat NOT RECORDED')]
    parts = [f'bench {name} (runs {runs})', f'arm {arm}', f'settings {uniq("settings")}', f'tier {uniq("tier")}',
             f'res {uniq("res")} (draw scale {uniq("scale")})']
    for s in steps:
        for w in s['windows']:
            n = w['n']
            parts.append(f"{w['label']} [frames {w['win'][0]}..{w['win'][1]}]: {n['basis']} {n['fps']:.1f} fps, 1% low {fmt(n['low1'])}{(' (worst-gap-per-row ' + fmt(n['low1_max']) + ')') if n.get('low1_max') is not None else ''} | "
                         f"gpu {fmt(n['gpu'])} render {fmt(n['render']) if n['render'] is not None else 'not measured'}{(' record ' + fmt(n['record'])) if n.get('record') is not None else ''} swap {fmt(n['swap'])} ms | {w['heat']}" + (f", {burst_rules.norm_text(n['fps'], w['thermal'])}" if w.get('thermal') and w['thermal'].get('perf') else ''))
    line = ' | '.join(parts)
    if bad:
        line = 'NOT A BENCH NUMBER: ' + line
    return line, steps, name


def split_line(w):
    """S102 H3: 'split [bryan frames A..B]: reader_draw 8.1/9.3 ... (mean / mean of the N 1%-low frames)' or 'split [...]: not measured ...'."""
    n = w['n']
    head = f"split [{w['label']} frames {w['win'][0]}..{w['win'][1]}]"
    if not n.get('split'):
        return f'{head}: not measured (this csv has no fs_ columns: run with --wwe2k14_frame_split=true)'
    parts = []
    for c in SPLIT_COLS:
        m, t = n['split'][c]
        parts.append(f"{c[3:-3]} {fmt(m, '{:.2f}')}/{fmt(t, '{:.2f}')}")
    return f"{head}: " + ' '.join(parts) + f" ms (mean / mean of the {n['split_tail_n']} 1%-low frames)"


LEDGER_HEAD = ['scene', 'side', 'kind', 'lane', 'lo', 'hi', 'unit', 'source', 'when', 'note']


def append_ledger(path, steps, name, when):
    p = Path(path)
    new = not p.exists() or p.stat().st_size == 0
    with open(p, 'a', newline='', encoding='utf-8') as f:
        w = csv.writer(f, lineterminator='\n')
        if new:
            w.writerow(LEDGER_HEAD)
        for s in steps:
            for win in s['windows']:
                if '/' in win['label']:
                    continue   # the labelled extra windows (tron) are not ledger scenes
                n = win['n']
                for side, unit in (('gpu', 'ms'), ('render', 'ms'), ('swap', 'ms'), ('fps', 'fps'), ('low1', 'fps')):
                    v = n.get(side)
                    if v is None:
                        continue
                    w.writerow([s['label'], side, 'bench', '', f'{v:.2f}', f'{v:.2f}', unit,
                                f'bench {name} run {s["run"]} frames {win["win"][0]}..{win["win"][1]}', when,
                                f'{s["settings"]} {s["tier"]} {s["res"]} {win["heat"]}'.replace('\n', ' ')])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run_dir')
    ap.add_argument('--ledger', default='')
    ap.add_argument('--when', default='')
    ap.add_argument('--run', type=int, default=None, help='a run number instead of the newest of each step')
    ap.add_argument('--allow-no-heat', action='store_true')
    ap.add_argument('--split', action='store_true', help='S102 H3: also print the swap-thread split (means, and means over the 1 %%-low frames) of each window')
    a = ap.parse_args()
    try:
        line, steps, name = line_for(a.run_dir, a.run, a.allow_no_heat)
    except Refused as e:
        print(f'REFUSED: {e}', file=sys.stderr)
        return 2
    print(line)
    wer_report.print_wer(a.run_dir)   # S102 TS1: the job's GPU watchdog reports (nothing printed for a folder without a listing and without a loss)
    if a.split:
        for s in steps:
            for w in s['windows']:
                print(split_line(w))
    if a.ledger:
        if not a.when:
            print('REFUSED: --ledger needs --when (read the clock)', file=sys.stderr)
            return 2
        if line.startswith('NOT A BENCH NUMBER'):
            print('REFUSED: --ledger will not record a number without its heat', file=sys.stderr)
            return 2
        append_ledger(a.ledger, steps, name, a.when)
    return 0


if __name__ == '__main__':
    sys.exit(main())
