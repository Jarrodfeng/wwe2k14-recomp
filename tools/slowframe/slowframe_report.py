"""Slow-frame report (Session 8 task 12): one short report per slow frame of a run made with --wwe2k14_slowframe_ms.

    py tools/slowframe/slowframe_report.py RUN [--logs DIR] [--dir DIR] [--top 8] [--context 120] [--quiet]

Input (all our own run output): out/harness/slowframes/RUN/events.csv + frame_<N>_stacks.txt (written in-app by
src/slow_frame.h), and the run's wwe2k14_RUN.log, frames_RUN.csv, timeline_RUN.csv and anatomy_RUN_K*.csv.
Output: out/harness/slowframes/RUN/report_<N>.txt per slow frame, anatomy_<N>.txt (the full anatomy_report.py output
of the capture that followed it) and summary.txt (also printed). Everything stays under out/harness (gitignored).

Per slow frame N (frames_RUN.csv numbering: frame N = VdSwap N-1 to VdSwap N):
  scene        bench_report.py's scenes (tron_NNN / match / mark:NAME) containing N, the last input-script mark and
               the last files opened before it
  stacks       profile_stacks.py on the in-app ring dump: the main thread's and the GPU Commands thread's samples
               taken while frame N was built (tag N-1, the same wall-clock window for both threads; the GPU thread
               is then usually still working on earlier frames' commands): top stacks, self and inclusive time
  CP frame     bench_report.py's per-CP-frame table for the command-processor frame that presents guest frame N:
               busy / idle / WAIT_REG_MEM / host-GPU wait, GPU ms (gpu_ts: sum of the frame's submissions'
               GPU time) and the per-frame cause counters (cpf_*: draws, texture loads, barrier batches, uploads,
               ...), each against the median of the previous --context CP frames: the "unusual" columns are the
               cheap rolling record of what this frame did differently
  main waits   the game's GPU waits (gwait, by reason code) and GPU kicks during frame N (timeline)
  anatomy      the anatomy capture (sparse GPU marks) requested at frame N. It starts at the command processor's
               next frame boundary. The CP runs behind VdSwap, so its first frame can be frame N's own CP frame
               (flagged: frame N's CP / GPU numbers then include the capture's overhead) or a later one; the guest
               frame of each capture frame is shown (the CP frame that ends at the cp_swap just before the
               capture's 'anatomy' timeline event). anatomy_report.py's GPU totals, GPU causes and top stalls.
  summary      over-budget frames within the capture window of a report (frames N+1 .. the last captured frame
               + 2) are counted apart: the capture's own row writing and marks can make them slow (run 282).
"""
import argparse
import bisect
import contextlib
import csv
import io
import os
import re
import statistics
import sys
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, 'tools', 'bench'))
sys.path.insert(0, os.path.join(REPO, 'tools', 'analysis'))
import anatomy_report  # noqa: E402
import bench_report  # noqa: E402
import profile_stacks  # noqa: E402

sys.path.insert(0, os.path.join(REPO, 'tools', 'q'))
try:
    import q as qtool  # noqa: E402  (db/*.json names for guest functions)
except Exception:  # noqa: BLE001 - names are a convenience; reports work without them
    qtool = None

DEFAULT_LOGS = bench_report.DEFAULT_LOGS
DEFAULT_ROOT = os.path.join(REPO, 'out', 'harness', 'slowframes')

# Per-CP-frame counters the timeline records right before cp_swap, beyond bench_report.COUNTERS (SDK patch, Sessions
# 7b/7c; the comments next to their Record calls). Added to bench_report's table so its parser reads them too.
EXTRA_COUNTERS = [
    ('cpf_sync', ('barrier_batches', 'rendering_restarts', 'dispatches')),
    ('cpf_sync2', ('tex_loads', 'tex_loads_scaled', 'fsi_edram_barriers')),
    ('cpf_texload', ('texload_loads', 'texload_scaled', 'texload_kib')),
    ('cpf_texdirect', ('texdirect_loads', 'texdirect_fallbacks', 'texdirect_mismatches')),
    ('cpf_up', ('up_calls', 'up_pages_skipped', 'up_pages_stale')),
    ('cpf_up2', ('up_pages', 'up_hashed_kib', 'up_hash_us')),
]
for _name, _cols in EXTRA_COUNTERS:
    bench_report.COUNTERS.setdefault(_name, _cols)

# CP-frame columns compared against the rolling median (times in ms, then counts).
TIME_COLS = ['span', 'busy', 'idle', 'wrm', 'hwait', 'intr', 'drain', 'asub', 'gpu_ms']
COUNT_COLS = ['draws', 'copies', 'tex_loads', 'tex_loads_scaled', 'texload_kib', 'barrier_batches',
              'rendering_restarts', 'dispatches', 'fsi_edram_barriers', 'up_calls', 'up_pages', 'pipeline_changes',
              'tex_sets', 'const_sets', 't0_regs', 'rr_calls', 'rr_kib', 'submissions']
WAIT_LEAVES = ('NtWaitFor', 'NtDelayExecution', 'SleepEx', 'SwitchToThread', 'NtYieldExecution', 'WaitForSingleObject',
               'WaitForMultipleObjects', 'SleepConditionVariable', 'RtlSleepConditionVariable', 'NtWaitForAlertByThreadId',
               'ZwWaitFor', 'NtRemoveIoCompletion')
ANATOMY_STARTED = re.compile(r'Frame anatomy capture (\d+) started: (\d+) frames to (.+?)\s*$')
SLOWFRAME_ON = re.compile(r'\[wwe2k14\]\[slowframe\] on: budget ([\d.]+) ms')
# The game's calls to its frame-rate halve / restore functions, as pacing_hooks.h logs them ("frame 5032: restore
# call at ... changes the word 30 -> 60", or "word 60: no change" when the word already had that value, e.g. with the
# 4K preset's entrance 60 fps experiment). They mark entrance starts / ends; slow frames this close to one are
# marked as transitions.
PACING_CALL = re.compile(r'\[wwe2k14\]\[pacing\] frame (\d+): (halve|restore) call at \S+ \S+(?:,)? '
                         r'(?:changes the word (\d+) -> (\d+)|word (\d+): no change)')
PACING_SKIP = re.compile(r'\[wwe2k14\]\[pacing\] entrance 60 fps experiment: skipping the halve call at \S+ '
                         r'\(frame (\d+)\)')
PACING_NEAR = 8


def short(name, width=90):
    return name if len(name) <= width else name[:width - 3] + '...'


_DB_NAMES = {}
GUEST_NAME = re.compile(r'^(?:__imp__)?sub_([0-9A-Fa-f]{8})$')
CONF_RANK = {'high': 0, 'medium': 1, 'low': 2, 'unknown': 3}


def db_name(name):
    """'sub_82C3BAE0' -> 'sub_82C3BAE0 [frame_sync_wait_clients_done]' when db/*.json has an entry (the most
    confident one); other names unchanged."""
    m = GUEST_NAME.match(name)
    if not m or qtool is None:
        return name
    if not _DB_NAMES:
        best = {}
        for a, _, e in qtool.load_db_entries():
            rank = CONF_RANK.get(str(e.get('confidence', 'unknown')).lower(), 3)
            if e.get('name') and (a not in best or rank < best[a][0]):
                best[a] = (rank, e['name'])
        _DB_NAMES.update({a: n for a, (r, n) in best.items()})
        _DB_NAMES.setdefault(-1, None)  # loaded marker
    label = _DB_NAMES.get(int(m.group(1), 16))
    return f'{name} [{label}]' if label else name


def load_events(path):
    out = []
    with open(path, newline='') as f:
        for r in csv.DictReader(f):
            try:
                out.append(dict(frame=int(r['frame']), t=float(r['t_seconds']), ms=float(r['frame_ms']),
                                report=int(r['report']), stacks=r['stacks_file'], anatomy=int(r['anatomy_frames']),
                                note=r['anatomy_note']))
            except (ValueError, KeyError, TypeError):
                break  # truncated last line
    return out


def stacks_for(path, thread, lo, hi):
    """profile_stacks.load -> (samples, inclusive Counter, self Counter, [(names, count)])."""
    data, _, _ = profile_stacks.load(path, thread, lo, hi)
    total = sum(c for _, _, c in data)
    incl, self_ = Counter(), Counter()
    stacks = Counter()
    for names, _, count in data:
        self_[names[0]] += count
        for n in set(names):
            incl[n] += count
        stacks[tuple(names)] += count
    return total, incl, self_, stacks


SYSTEM_DLLS = ('ntdll.dll!', 'KERNELBASE.dll!', 'kernel32.dll!', 'KERNEL32.DLL!', 'ucrtbase.dll!', 'msvcp140.dll!')


def compact(name, width=60):
    """A shorter name for one-line summaries: MSVC-decorated names ('?Fn@Class@ns@@...') -> 'ns::Class::Fn'."""
    if name.startswith('?') and '@@' in name and not name.startswith('??'):
        parts = name[1:].split('@@', 1)[0].split('@')
        name = '::'.join(reversed(parts))
    return short(db_name(name), width)


def culprit(self_, stacks, depth=3):
    """The hottest leaf and its most common callers: 'leaf <- caller <- ...'. System DLL frames after the leaf
    (wait / sleep wrappers) are skipped, so a waiting leaf shows who waited."""
    if not self_:
        return None, 0
    leaf, n = self_.most_common(1)[0]
    paths = Counter()
    for names, count in stacks.items():
        if names[0] == leaf:
            callers = [x for x in names[1:] if not x.startswith(SYSTEM_DLLS)]
            paths[(leaf,) + tuple(callers[:depth])] += count
    path = paths.most_common(1)[0][0] if paths else (leaf,)
    return ' <- '.join(compact(p) for p in path), n


def stack_section(lines, path, thread, tag, top):
    total, incl, self_, stacks = stacks_for(path, thread, tag, tag)
    if not total:
        lines.append(f'  {thread}: no samples tagged {tag}')
        return None
    lines.append(f'  {thread}: {total} samples in this frame')
    path_text, n = culprit(self_, stacks)
    lines.append(f'    hottest leaf + callers ({100 * n / total:.0f}%): {path_text}')
    lines.append(f'    top stacks (leaf first, up to 6 frames):')
    for names, count in stacks.most_common(3):
        lines.append(f'      {100 * count / total:5.1f}%  ' + ' <- '.join(short(db_name(x), 60) for x in names[:6]))
    lines.append(f'    self (leaf) time:')
    for name, count in self_.most_common(top):
        lines.append(f'      {100 * count / total:5.1f}%  {short(db_name(name), 110)}')
    roots = [nm for nm, c in incl.items() if c >= 0.99 * total]
    lines.append(f'    inclusive time (functions on >= 99% of samples left out: {len(roots)} thread roots):')
    shown = 0
    for name, count in incl.most_common():
        if count >= 0.99 * total:
            continue
        lines.append(f'      {100 * count / total:5.1f}%  {short(db_name(name), 110)}')
        shown += 1
        if shown >= top:
            break
    kinds = classify_samples(stacks)
    lines.append('    samples by kind: ' + ', '.join(
        f'{k} {100 * v / total:.0f}%' for k, v in kinds['counts'].most_common()) +
        (f'; guest kernel waits called from {", ".join(kinds["kwait_from"])}' if kinds['kwait_from'] else ''))
    return dict(samples=total, culprit=path_text, culprit_pct=100 * n / total,
                frac={k: v / total for k, v in kinds['counts'].items()}, kwait_from=kinds['kwait_from'])


# Sample kinds for the verdict. GPU_WAIT: the function every game GPU-wait loop polls (sub_82CB1F50) and the fence
# wait loop (sub_82CB5A20), both found in generated code in Session 5 (src/gpu_timeline.h). The rest go by host
# frame names.
GPU_WAIT = ('sub_82CB1F50', 'sub_82CB5A20')


def is_guest(name):
    return name.startswith(('sub_', '__imp__sub_', '__imp____'))


def classify_samples(stacks):
    """Counter of sample kinds: forced stall (debug knob) / game GPU-wait poll / guest kernel wait (an xboxkrnl
    wait called by guest code) / other host wait / guest code (leaf in recompiled code) / host code; plus the guest
    functions that called the kernel waits (most common first)."""
    counts, callers = Counter(), Counter()
    for names, count in stacks.items():
        leaf = names[0]
        joined = ' '.join(names)
        if 'SlowFrameForcedStall' in joined or 'SlowFrameReporter' in joined:
            kind = 'forced stall'
        elif any(g in x for x in names for g in GPU_WAIT):
            kind = 'game GPU wait'
        elif any(w in leaf for w in WAIT_LEAVES) and 'rex::kernel::xboxkrnl::' in joined:
            kind = 'guest kernel wait'
            guest = [db_name(x.replace('__imp__', '')) for x in names if is_guest(x)][:3]
            if guest:
                callers[' <- '.join(guest)] += count
        elif any(w in leaf for w in WAIT_LEAVES):
            kind = 'host wait/yield'
        elif is_guest(leaf):
            kind = 'guest code'
        else:
            kind = 'host code'
        counts[kind] += count
    return dict(counts=counts, kwait_from=[c for c, _ in callers.most_common(1)])


def verdict(frame_ms, main, gpu_thread, cpf, gw):
    """One-line reading from measured numbers (rules below, nothing inferred beyond them)."""
    if not main:
        return 'no main-thread samples'
    f = main.get('frac', {})
    parts = []
    if f.get('forced stall', 0) >= 0.5:
        return 'forced stall (debug knob wwe2k14_slowframe_force_every)'
    gwait_ms = gw.get('gwait', 0) if gw else 0
    if f.get('guest kernel wait', 0) >= 0.4:
        who = ', '.join(main.get('kwait_from') or []) or '?'
        parts.append(f'main thread blocked in a guest kernel wait {100 * f["guest kernel wait"]:.0f}% of the frame '
                     f'(called from {who}; what it waits for: unconfirmed)')
    if gwait_ms >= 0.4 * frame_ms or f.get('game GPU wait', 0) >= 0.4:
        parts.append(f'main thread waited for the GPU (gwait {gwait_ms:.1f} ms, GPU-wait samples '
                     f'{100 * f.get("game GPU wait", 0):.0f}%)')
    if f.get('guest code', 0) >= 0.5:
        parts.append(f'main thread busy in guest code ({100 * f["guest code"]:.0f}%)')
    if cpf:
        span = cpf.get('span') or 0
        if span and (cpf.get('busy') or 0) >= 0.7 * span:
            parts.append(f'CP busy {cpf["busy"]:.1f} of {span:.1f} ms (CP-bound)')
        elif span and (cpf.get('idle') or 0) >= 0.5 * span:
            parts.append(f'CP idle {cpf["idle"]:.1f} of {span:.1f} ms (waiting for the game\'s commands)')
        gpu_bound = cpf.get('gpu_ms') is not None and cpf['gpu_ms'] >= 0.7 * frame_ms
        if gpu_bound:
            parts.append(f'host GPU {cpf["gpu_ms"]:.1f} ms (GPU-bound)')
        waited_gpu = gwait_ms >= 0.4 * frame_ms or f.get('game GPU wait', 0) >= 0.4
        if waited_gpu and span and (cpf.get('busy') or 0) < 0.5 * span and not gpu_bound:
            parts.append(f'but neither the CP (busy {cpf.get("busy") or 0:.1f} ms) nor the host GPU '
                         f'({cpf.get("gpu_ms") or 0:.1f} ms) was busy for most of it: a pacing / handshake wait (CP '
                         f'WAIT_REG_MEM {cpf.get("wrm") or 0:.1f} ms, idle {cpf.get("idle") or 0:.1f} ms)')
    return '; '.join(parts) or 'unclassified (see the stacks)'


def gwaits(run, frame):
    """Main-thread gwait ms by reason code and GPU kicks during guest frame `frame` (VdSwap frame-1 to frame)."""
    if not run.ev or not run.meta.get('vd_times'):
        return None
    vd = run.meta['vd_times']
    frames = [f for _, f in vd]
    i = bisect.bisect_left(frames, frame)
    if i >= len(vd) or i == 0 or vd[i][1] != frame:
        return None
    t0, t1 = vd[i - 1][0], vd[i][0]
    main_th = run.meta.get('threads', {}).get('main')
    by = Counter()
    for a, b, begin in bench_report.pair_scopes(run.ev, 'gwait', main_th):
        if b <= t0 or a >= t1:
            continue
        by[bench_report.hx(begin[3])] += min(b, t1) - max(a, t0)
    return dict(ms=(t1 - t0) / 1e6, by_reason={k: v / 1e6 for k, v in by.items()},
                kicks=run.meta.get('guest_kicks', {}).get(frame))


def anatomy_captures(run):
    """Capture index -> dict(path, frames requested, guest frames covered by each capture frame)."""
    caps = {}
    for part in run.log['parts']:
        with open(part, encoding='utf-8', errors='replace') as f:
            for line in f:
                m = ANATOMY_STARTED.search(line)
                if m:
                    caps[int(m.group(1))] = dict(path=m.group(3), requested=int(m.group(2)), guest={}, t_first=None)
    if run.ev and run.cp:
        cps = run.meta.get('_cps') or [x[0] for x in run.ev.get('cp_swap', ())]
        for x in run.ev.get('anatomy', ()):
            idx, cframe = bench_report.hx(x[3]), bench_report.hx(x[4])
            c = caps.setdefault(idx, dict(path=None, requested=None, guest={}, t_first=None))
            if c['t_first'] is None or x[0] < c['t_first']:
                c['t_first'] = x[0]
            # The SDK records 'anatomy' in AnatomyFrameBoundary, AFTER the cp_swap that ends the captured frame
            # (command_processor.cpp: Record("cp_swap") then AnatomyFrameBoundary). So the captured frame is the
            # one ending at the last cp_swap at or before the event: cps[j-1] with j = bisect_right, which is
            # run.cp[j-2] (run.cp[k] is the CP frame cps[k] .. cps[k+1]). Not bench_report's bisect_left / j-1,
            # which is for the cpf_* events recorded BEFORE their cp_swap.
            j = bisect.bisect_right(cps, x[0])
            if 2 <= j <= len(run.cp) + 1:
                c['guest'][cframe] = run.cp[j - 2]['guest_frame']
    return caps


def capture_window(ev, cap):
    """Guest frames after slow frame N whose length the capture requested at N can have changed: N+1 .. the last
    captured guest frame + 2 (its rows are written at the next CP frame boundary, and the game feels a late CP a
    frame or so later), and always at least N+1 .. N+anatomy+2 (also when the mapping is missing)."""
    n = ev['frame']
    if ev['anatomy'] <= 0:
        return range(0)
    captured = [g for g in (cap or {}).get('guest', {}).values() if g is not None]
    hi = max([n + ev['anatomy']] + captured) + 2
    return range(n + 1, hi + 1)


def pick_capture(caps, run, frame):
    """The first capture whose first frame ended after VdSwap `frame` (the request was made at that VdSwap)."""
    vd = dict((f, t) for t, f in run.meta.get('vd_times', ())) if run.meta else {}
    t_req = vd.get(frame)
    best = None
    for idx, c in sorted(caps.items()):
        if c['t_first'] is None or t_req is None or c['t_first'] < t_req:
            continue
        if best is None or c['t_first'] < caps[best]['t_first']:
            best = idx
    return best


def run_anatomy_report(csv_path):
    """anatomy_report.py <csv> --gpu --top 5, captured. Returns its text."""
    buf = io.StringIO()
    argv = sys.argv
    sys.argv = ['anatomy_report.py', csv_path, '--gpu', '--top', '5']
    try:
        with contextlib.redirect_stdout(buf):
            anatomy_report.main()
    except Exception as e:  # noqa: BLE001 - report the failure in the text, keep the other reports going
        buf.write(f'\nanatomy_report.py failed: {type(e).__name__}: {e}\n')
    finally:
        sys.argv = argv
    return buf.getvalue()


def section(text, title, max_lines):
    """Lines of an anatomy_report section ('== title...') up to the next blank line."""
    out, on = [], False
    for line in text.splitlines():
        if line.startswith('== ' + title) or line.startswith('\n== ' + title):
            on = True
            out.append(line)
            continue
        if on:
            if not line.strip() or line.startswith('=='):
                break
            out.append(line)
            if len(out) > max_lines:
                break
    return out


CAUSE_ROW = re.compile(r'^\s+(.+?)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)%')


def top_gpu_causes(text, n=2):
    rows = []
    for line in section(text, 'GPU causes', 40)[3:]:
        m = CAUSE_ROW.match(line)
        if m:
            rows.append((float(m.group(3)), m.group(1).strip(), float(m.group(4))))
    rows.sort(reverse=True)
    return [f'{cat} {ms_:.2f} ms ({share:.0f}%)' for ms_, cat, share in rows[:n]]


def unusual(cp_frame, history):
    """Columns of this CP frame far above the median of the previous frames: [(col, value, median, ratio)]."""
    out = []
    for col in TIME_COLS + COUNT_COLS:
        v = cp_frame.get(col)
        hist = [h.get(col) for h in history if h.get(col) is not None]
        if v is None or len(hist) < 10:
            continue
        med = statistics.median(hist)
        floor = 0.5 if col in TIME_COLS else 2  # ignore tiny absolute changes
        if v - med < floor:
            continue
        ratio = v / med if med > 0 else float('inf')
        if ratio >= 1.5:
            out.append((col, v, med, ratio))
    out.sort(key=lambda x: -x[3])
    return out


def fmt_val(col, v):
    return f'{v:.2f}' if col in TIME_COLS else f'{v:.0f}'


def scene_of(run, frame):
    names = [name for name, kind, s, e in run.scenes if s <= frame < e]
    mark = None
    for start, mid, name in run.marks:
        if start <= frame:
            mark = name
    opens = [(fr, p) for ts, p, fr in run.log['opens'] if fr is not None and fr < frame]
    last = opens[-2:]
    return names, mark, last


def report_frame(run, ev, sdir, caps, cp_by_guest, args):
    n = ev['frame']
    lines = []
    ms_csv = run.vd_ms.get(n)
    lines.append(f'# Slow frame {n} of run {run.n}: {ms_csv if ms_csv is not None else ev["ms"]:.1f} ms '
                 f'(in-app {ev["ms"]:.1f} ms, t={ev["t"]:.1f} s, report {ev["report"]})')
    names, mark, last = scene_of(run, n)
    lines.append(f'scene: {", ".join(names) if names else "(no bench scene)"}; last mark: {mark or "-"}; '
                 f'last files opened: ' + (', '.join(f'{os.path.basename(p.replace(chr(92), "/"))} '
                                                     f'(frame {fr}, {n - fr} before)' for fr, p in last) or '-'))
    neighbours = ' '.join(f'{f}:{run.vd_ms[f]:.1f}' for f in range(n - 3, n + 4) if run.vd_ms.get(f) is not None)
    lines.append(f'neighbouring frames (frame:ms): {neighbours}')
    out = dict(frame=n, ms=ms_csv if ms_csv is not None else ev['ms'], scene=names[0] if names else (
        'mark:' + mark if mark else '-'))
    # The capture requested at this frame (used by the CP section too: it can include frame N's own CP frame).
    cap_idx = pick_capture(caps, run, n) if ev['anatomy'] > 0 else None
    cap = caps.get(cap_idx) if cap_idx is not None else None
    captured = sorted(set(g for g in (cap or {}).get('guest', {}).values() if g is not None))
    out['anatomy_frames'] = captured
    out['anatomy_includes_n'] = n in captured
    out['window'] = capture_window(ev, cap)

    # Stacks.
    lines.append('')
    lines.append(f'== call stacks while frame {n} was built (samples tagged {n - 1}; profiler ring dump)')
    stacks_path = os.path.join(sdir, ev['stacks']) if ev['stacks'] else None
    if not stacks_path or not os.path.exists(stacks_path):
        lines.append(f'  no stacks file ({ev["stacks"] or "profiler off"}; the run may have ended before frame '
                     f'{n + 2})')
    else:
        per_tag = []
        for tag in range(n - 3, n + 2):
            _, per_thread, _ = profile_stacks.load(stacks_path, 'all', tag, tag)
            per_tag.append(f'{tag}: ' + ', '.join(f'{t} {c}' for t, c in sorted(per_thread.items())))
        lines.append('  samples per tag: ' + ' | '.join(per_tag))
        for thread, key in (('main thread', 'main'), ('GPU Commands', 'gpu')):
            out[key] = stack_section(lines, stacks_path, thread, n - 1, args.top)

    # CP frame from the timeline.
    lines.append('')
    cpf = cp_by_guest.get(n)
    if cpf is None:
        lines.append(f'== command-processor frame presenting guest frame {n}: not in the timeline '
                     f'({"no timeline" if not run.ev else "not paired"})')
    else:
        k = run.cp.index(cpf)
        history = [f for f in run.cp[max(0, k - args.context):k]]
        lines.append(f'== command-processor frame {cpf["cp_frame"]} (presents guest frame {n}; timeline, ms; '
                     f'"median" = previous {len(history)} CP frames)')
        lines.append('  ' + '  '.join(f'{c} {fmt_val(c, cpf[c])}' for c in TIME_COLS if cpf.get(c) is not None)
                     + (f'  lag {cpf["lag"]:.1f}' if cpf.get('lag') is not None else ''))
        counts = [c for c in COUNT_COLS if cpf.get(c) is not None]
        if counts:
            lines.append('  counters: ' + '  '.join(f'{c} {fmt_val(c, cpf[c])}' for c in counts))
        un = unusual(cpf, history)
        out['gpu_ms'] = cpf.get('gpu_ms')
        out['cp_busy'] = cpf.get('busy')
        out['unusual'] = un
        if un:
            lines.append('  unusual vs median (top): ' + ', '.join(
                f'{c} {fmt_val(c, v)} vs {fmt_val(c, m)} ({"new" if r == float("inf") else f"x{r:.1f}"})'
                for c, v, m, r in un[:8]))
        else:
            lines.append('  unusual vs median: none (no column >= 1.5x its median): the CP frame looks normal')
        if cpf.get('gpu_ms') is None:
            lines.append('  (no gpu_ts: run without --vulkan_gpu_timestamps=true?)')
        if out['anatomy_includes_n']:
            lines.append(f'  NOTE: this CP frame was itself captured (anatomy capture {cap_idx}, requested at this '
                         f'frame): its busy / GPU ms include the capture\'s per-draw recording and GPU marks')

    g = gwaits(run, n)
    if g:
        reasons = ', '.join(f'reason {r}: {v:.1f} ms' for r, v in sorted(g['by_reason'].items())) or 'none'
        lines.append(f'== main thread during frame {n} ({g["ms"]:.1f} ms in the timeline): GPU waits (gwait) {reasons}; '
                     f'GPU kicks {g["kicks"]}')
        out['gwait'] = sum(g['by_reason'].values())
    out['verdict'] = verdict(out['ms'], out.get('main'), out.get('gpu'), cpf, out)
    if cpf is not None and out['anatomy_includes_n']:
        out['verdict'] += ' [CP frame inside the anatomy capture: its CP / GPU numbers include the capture overhead]'
    lines.insert(3, f'reading (rules in verdict() of slowframe_report.py): {out["verdict"]}')

    # Anatomy capture requested at this frame.
    lines.append('')
    if ev['anatomy'] <= 0:
        lines.append(f'== anatomy: none ({ev["note"]})')
    else:
        idx, c = cap_idx, cap
        path = c['path'] if c else None
        if path and not os.path.exists(path):
            # The log has the absolute path of the run; the logs folder may have been moved or copied since.
            moved = os.path.join(args.logs, os.path.basename(path.replace(chr(92), '/')))
            if os.path.exists(moved):
                path = moved
        if not path or not os.path.exists(path):
            lines.append(f'== anatomy: requested ({ev["anatomy"]} frames) but no capture file found '
                         f'(capture {idx}, path {path})')
        else:
            covered = ', '.join(f'capture frame {cf} = guest frame {gf}' for cf, gf in sorted(c['guest'].items()))
            if out['anatomy_includes_n']:
                which = 'the CP was behind, so it includes the slow frame itself'
            elif captured and captured[0] > n:
                which = 'so it covers frames after the slow one'
            else:
                which = 'guest frames as mapped'
            lines.append(f'== anatomy capture {idx} (requested at frame {n}; starts at the CP\'s next frame '
                         f'boundary, {which}: {covered or "?"}) -> {path}')
            text = run_anatomy_report(path)
            full = os.path.join(sdir, f'anatomy_{n:06d}.txt')
            with open(full, 'w', encoding='utf-8') as f:
                f.write(text)
            for title, nmax in (('GPU totals', 6), ('GPU causes', 14), ('GPU stalls', 6)):
                sec = section(text, title, nmax)
                lines.extend('  ' + s for s in sec)
            lines.append(f'  (full anatomy_report.py --gpu output: {os.path.basename(full)})')
            out['anatomy_causes'] = top_gpu_causes(text)
    path = os.path.join(sdir, f'report_{n:06d}.txt')
    with open(path, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    return out, lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run')
    ap.add_argument('--logs', default=DEFAULT_LOGS)
    ap.add_argument('--dir', default=None, help='slow-frame folder (default out/harness/slowframes/RUN)')
    ap.add_argument('--top', type=int, default=8)
    ap.add_argument('--context', type=int, default=120, help='CP frames before the slow one for the median')
    ap.add_argument('--quiet', action='store_true', help='print only the summary')
    args = ap.parse_args()
    n = bench_report.run_id(args.run)  # '278', '0278', 'wwe2k14_278.log', 'timeline_278.csv' -> '278'
    sdir = args.dir or os.path.join(DEFAULT_ROOT, n)
    events_path = os.path.join(sdir, 'events.csv')
    if not os.path.exists(events_path):
        raise SystemExit(f'{events_path} not found (run without --wwe2k14_slowframe_ms, or no frame over budget)')
    events = load_events(events_path)
    run = bench_report.Run(n, args.logs, quiet=True)
    cp_by_guest = {f['guest_frame']: f for f in (run.cp or []) if f['guest_frame'] is not None}
    caps = anatomy_captures(run)

    budget = None
    pacing = []  # (frame, 'halve 60 -> 30' / 'restore (60, no change)'): the game's frame-rate calls, [pacing] lines
    skipped = set()
    for part in run.log['parts']:
        with open(part, encoding='utf-8', errors='replace') as f:
            for line in f:
                m = SLOWFRAME_ON.search(line)
                if m and budget is None:
                    budget = float(m.group(1))
                m = PACING_CALL.search(line)
                if m:
                    what = (f'{m.group(2)} {m.group(3)} -> {m.group(4)}' if m.group(3) else
                            f'{m.group(2)} (word {m.group(5)}, no change)')
                    pacing.append((int(m.group(1)), what))
                m = PACING_SKIP.search(line)
                if m:
                    skipped.add(int(m.group(1)))
    # A halve call the 4K preset's entrance 60 fps experiment skipped did not change the word.
    pacing = [(pf, what + (' (skipped by the entrance 60 fps experiment)' if pf in skipped and
                           what.startswith('halve') else '')) for pf, what in pacing]
    over = sorted(f for f, v in run.vd_ms.items() if v is not None and budget is not None and v >= budget)
    results = []
    for ev in events:
        res, lines = report_frame(run, ev, sdir, caps, cp_by_guest, args)
        near = [(pf, what) for pf, what in pacing if abs(pf - ev['frame']) <= PACING_NEAR]
        if near:
            res['pacing'] = near
            note = (f'within {PACING_NEAR} frames of a game frame-rate call (' +
                    ', '.join(f'{what} at frame {pf}' for pf, what in near) +
                    '; pacing log): an entrance start / end transition, not necessarily typical of the scene')
            lines.insert(4, note)
            with open(os.path.join(sdir, f'report_{ev["frame"]:06d}.txt'), 'w', encoding='utf-8') as f:
                f.write('\n'.join(lines) + '\n')
        results.append(res)
        if not args.quiet:
            print('\n'.join(lines))
            print()

    # Over-budget frames inside a capture window may have been made slow by the capture itself: counted apart.
    in_window = {}
    for r in results:
        for f in r.get('window', ()):
            in_window.setdefault(f, r['frame'])
    inside = [f for f in over if f in in_window]
    outside = [f for f in over if f not in in_window]
    by_scene = Counter()
    for f in outside:
        names = [name for name, kind, s, e in run.scenes if s <= f < e]
        by_scene[names[0] if names else '-'] += 1
    summary = [f'# Slow-frame summary, run {n}: budget {budget} ms; {len(over)} frames over budget in frames_{n}.csv: '
               f'{len(outside)} outside the anatomy capture windows '
               f'({", ".join(f"{k} {v}" for k, v in by_scene.most_common()) or "-"}), {len(inside)} inside one '
               f'(frames N+1 .. last captured + 2 of a report; the capture can make these slow itself: '
               + (', '.join(f'{f} ({run.vd_ms[f]:.1f} ms, after {in_window[f]})' for f in inside) or 'none') +
               f'); {len(events)} reported (rate limit / max in the log)',
               f'# per report: frame, ms, [scene]: reading; then main thread: hottest leaf <- callers | GPU Commands: same | '
               f'CP frame: GPU ms, busy ms, top unusual counters vs the previous frames | anatomy capture (its guest '
               f'frames; "incl. N" = it included the slow frame\'s own CP frame): top GPU causes']
    for r in results:
        main_t = r.get('main') or {}
        gpu_t = r.get('gpu') or {}
        un = r.get('unusual') or []
        un_text = ', '.join(f'{c} {fmt_val(c, v)} (median {fmt_val(c, m)})' for c, v, m, _ in un[:3]) or 'none'
        gpu_ms = f'{r["gpu_ms"]:.1f}' if r.get('gpu_ms') is not None else '?'
        busy = f'{r["cp_busy"]:.1f}' if r.get('cp_busy') is not None else '?'
        anat = '; '.join(r.get('anatomy_causes') or []) or '-'
        transition = (' [near frame-rate call: ' + ', '.join(f'{w} at {pf}' for pf, w in r['pacing']) + ']'
                      if r.get('pacing') else '')
        summary.append(f'frame {r["frame"]} {r["ms"]:.1f} ms [{r["scene"]}]{transition}: {r.get("verdict", "-")}')
        summary.append(
            f'    main: '
            f'{main_t.get("culprit", "-")} ({main_t.get("culprit_pct", 0):.0f}%) | GPU Commands: '
            f'{gpu_t.get("culprit", "-")} ({gpu_t.get("culprit_pct", 0):.0f}%) | CP: GPU {gpu_ms} ms, busy {busy} ms, '
            f'unusual: {un_text} | anatomy (frames {",".join(str(x) for x in r.get("anatomy_frames", [])) or "-"}'
            f'{"; incl. N" if r.get("anatomy_includes_n") else ""}): {anat}')
    with open(os.path.join(sdir, 'summary.txt'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(summary) + '\n')
    print('\n'.join(summary))
    print(f'\nreports: {sdir}')


if __name__ == '__main__':
    main()
