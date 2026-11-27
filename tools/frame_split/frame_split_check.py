#!/usr/bin/env python3
"""S102 lane H3 check (offline, 0 game runs, 0 Surface runs): the swap-thread split columns of frames_NNN.csv (--wwe2k14_frame_split).

  py -I tools\\frame_split\\frame_split_check.py [--root DIR]

1. Builds tools\\frame_split\\frame_split_test.cpp against the SHIPPED src\\frame_split.h + frame_split_math.h and runs it: the arithmetic (synthetic ReaderTimes /
   fence / barrier counters), the hook gatherers (a walk, a fence wait inside it, a barrier wait outside, another thread's wait) and the cost. Needs exit 0,
   'all checks passed', no FAIL line, and prints the measured cost (ns a frame, on and off).
2. Planted twins on copies of the math header (out\\frame_split\\plant_N\\): (1) a part counted twice (the reader's draw time left inside the replay), (2) a window
   snapshot not reset (last_ = now removed), (3) the fence counted twice. Each must make the test exit 1 with a FAIL line; a plant that no longer applies is its own
   failure ('DID NOT APPLY').
3. The csv layout: src\\frame_stats.h's header literal and format are read as text: the 21 existing columns first, in order, then the split columns appended by the loop over
   kColumnNames; the row's format has one more %s than before (the split cells) and ends in \\n. Planted twin (4): a split column put BEFORE an existing one in the
   header text must be flagged.
4. The parser (tools\\surface\\bench_line.py): reads the OLD csv of out\\s102\\ab5\\dh540_dm\\x (no split columns; 59.6 fps / 1 % low 44.7, R14's numbers) unchanged, prints
   'not measured' for --split on it; reads a synthetic csv with the 7 columns appended and returns the planted means and p99-frame means; REFUSES (Refused) a csv with
   a split column inserted before an existing one (planted twin 5), and one with an unknown column in a known position.
A check that cannot read its input fails loudly: a missing clang, test, header or old csv is a FAIL, never a pass."""
import argparse
import csv
import os
import re
import shutil
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
OLD_X = os.path.join('out', 's102', 'ab5', 'dh540_dm', 'x')


def main_checkout(root):
    out = subprocess.run(['git', '-C', root, 'rev-parse', '--git-common-dir'], capture_output=True, text=True)
    if out.returncode:
        return None
    return os.path.dirname(os.path.abspath(os.path.join(root, out.stdout.strip())))


class Fail(Exception):
    pass


def build_and_run(root, tag, math_text=None):
    d = os.path.join(root, 'out', 'frame_split', tag)
    os.makedirs(d, exist_ok=True)
    for name in ('frame_split.h', 'frame_split_math.h'):
        shutil.copy(os.path.join(root, 'src', name), os.path.join(d, name))
    if math_text is not None:
        with open(os.path.join(d, 'frame_split_math.h'), 'w', encoding='utf-8', newline='') as f:
            f.write(math_text)
    exe = os.path.join(d, 'frame_split_test.exe')
    if os.path.exists(exe):
        os.remove(exe)
    src = os.path.join(root, 'tools', 'frame_split', 'frame_split_test.cpp')
    if not os.path.isfile(src):
        raise Fail('missing ' + src)
    b = subprocess.run([CLANG, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-I', d, '-o', exe, src], capture_output=True, text=True)
    if b.returncode or not os.path.isfile(exe):
        raise Fail('frame_split_test (%s) did not compile\n%s%s' % (tag, b.stdout, b.stderr))
    r = subprocess.run([exe], capture_output=True, text=True, timeout=300)
    return r.returncode, r.stdout


def part1(root):
    code, text = build_and_run(root, 'shipped')
    print(text.rstrip())
    passes = len(re.findall(r'^PASS ', text, re.M))
    if code != 0 or 'all checks passed' not in text or re.search(r'^FAIL ', text, re.M) or passes < 12:
        raise Fail('the shipped arithmetic test: exit %d, %d PASS lines (need >= 12, none FAIL, "all checks passed")' % (code, passes))
    m = re.search(r'cost on: (\d+) ns per frame', text)
    if not m:
        raise Fail('the test printed no cost line')
    print('ok   shipped test: %d PASS, instrument on costs %s ns a frame' % (passes, m.group(1)))


def plants(root):
    path = os.path.join(root, 'src', 'frame_split_math.h')
    orig = open(path, encoding='utf-8', newline='').read()
    table = [
        ('part counted twice (the reader draw time left inside the replay)',
         'ToMs(d.walk) - ToMs(d.reader_draw_in_walk) - ', 'ToMs(d.walk) - '),
        ('window snapshot not reset (last_ = now removed)', '    last_ = now;\n', '    (void)now;\n'),
        ('fence counted twice (the in-walk part added again)', 'row.ms[4] = ToMs(d.fence);', 'row.ms[4] = ToMs(d.fence + d.fence_in_walk);'),
        ('F1: the pre-review replay formula (walk minus the WHOLE draw / present / scene end: the present and EndFrame run after the walk, so replay reads low)',
         'ToMs(d.walk) - ToMs(d.reader_draw_in_walk) - ToMs(d.reader_present_in_walk) - ToMs(d.scene_end_in_walk) - ToMs(d.fence_in_walk)',
         'ToMs(d.walk) - draw - present - scene_end - ToMs(d.fence_in_walk)'),
        ('F2: the running total not banked (RunningTicks::Bank does nothing: a census reset drops the reader to 0)',
         'draw += cur_draw, present += cur_present;', '(void)cur_draw, (void)cur_present;'),
        ('F4: an unknown thread CPU written as -1 (a made-up number in the mean)', 'std::nan("")', '-1.0'),
    ]
    for i, (name, a, b) in enumerate(table, 1):
        a_ = a.replace('\n', '\r\n') if '\r\n' in orig else a
        b_ = b.replace('\n', '\r\n') if '\r\n' in orig else b
        planted = orig.replace(a_, b_)
        if planted == orig:
            raise Fail('plant %d (%s): DID NOT APPLY (the line it edits changed)' % (i, name))
        code, text = build_and_run(root, 'plant_%d' % i, planted)
        if code != 1 or not re.search(r'^FAIL ', text, re.M):
            raise Fail('plant %d (%s): the test did NOT catch it (exit %d)' % (i, name, code))
        print('ok   plant %d caught: %s (%d FAIL lines)' % (i, name, len(re.findall(r'^FAIL ', text, re.M))))


def literal_header(text):
    """The frames csv header the code writes: the string literals of the std::fputs( call that opens it, joined."""
    i = text.find('std::fputs("frame,')
    if i < 0:
        raise Fail('src/frame_stats.h: no std::fputs("frame,... header')
    j = text.find('csv_);', i)
    return ''.join(re.findall(r'"([^"]*)"', text[i:j]))


def layout_problems(stats_text, math_text, order):
    """-> list of problems with the csv layout in frame_stats.h (read as text)."""
    probs = []
    nb = len([c for c in order if not c.startswith('fs_')])  # S102 D1: the base columns are the parser order's non-fs_ columns (21, then 22 with device_lost)
    base = order[:nb]
    h = literal_header(stats_text)
    if h.split(',') != base:
        probs.append('the header literal is not the %d existing columns in order: %s' % (nb, h[:120]))
    names = re.findall(r'"(fs_\w+)"', math_text)
    if names != order[nb:]:
        probs.append('kColumnNames %s != the parser order %s' % (names, order[nb:]))
    m = re.search(r'std::fprintf\(csv_, "(%llu[^"]*)"', stats_text)
    if not m:
        probs.append('no fprintf(csv_, "%llu... row format')
    else:
        fmt = m.group(1)
        if len(re.findall(r'%[-+ #0-9.]*(?:ll)?[a-zA-Z]', fmt)) != nb + 1 or not fmt.endswith('%s\\n'):
            probs.append('the row format must be the %d base conversions + one %%s (the split cells) + \\n: ' % nb + fmt)
    # the loop that appends the names must come after the base literal in the file, and the cvar read decides split_csv_ before the header
    if stats_text.find('kColumnNames') < stats_text.find('std::fputs("frame,'):
        probs.append('the split names are written before the base header')
    return probs


def part3(root):
    sdir = os.path.join(root, 'tools', 'surface')
    sys.path.insert(0, sdir)
    import bench_line
    order = bench_line.CSV_ORDER
    stats = open(os.path.join(root, 'src', 'frame_stats.h'), encoding='utf-8', newline='').read()
    math_ = open(os.path.join(root, 'src', 'frame_split_math.h'), encoding='utf-8', newline='').read()
    probs = layout_problems(stats, math_, order)
    if probs:
        raise Fail('csv layout: ' + '; '.join(probs))
    print('ok   csv layout: the 21 existing columns first, the 7 fs_ columns appended, the row format +1 %s')
    # planted twin 4: a split column in FRONT of an existing one in the header literal
    planted = stats.replace('std::fputs("frame,t_seconds,frame_ms,', 'std::fputs("frame,fs_rest_ms,t_seconds,frame_ms,')
    if planted == stats:
        raise Fail('plant L1: DID NOT APPLY (the header literal changed)')
    if not layout_problems(planted, math_, order):
        raise Fail('plant L1: a split column before an existing one was NOT flagged')
    print('ok   plant L1 caught: a split column in front of an existing column')
    return bench_line, order


def synth_csv(path, header, rows):
    with open(path, 'w', newline='', encoding='utf-8') as f:
        w = csv.writer(f, lineterminator='\n')
        w.writerow(header)
        for r in rows:
            w.writerow(r)


def vcs_out(root, *args):
    r = subprocess.run(['git', '-C', root] + list(args), capture_output=True, text=True)
    return r.stdout if r.returncode == 0 else None


def synth_extra_checks(root, bench_line, order, tmp, rows):
    """F4 / F5 / F7: other columns are read by name (a new or an old header is not refused); an unknown-CPU row (empty rest cell) leaves the mean unchanged; the
    new bench_line reads an OLD-header csv exactly as the base branch's script did; every historic header literal of src/frame_stats.h is accepted."""
    # F5: headers with extra / missing base columns are accepted; split columns still at the end
    for name, hdr in (('extra_mid', order[:5] + ['brand_new_col'] + order[5:]), ('extra_before_fs', order[:21] + ['brand_new_col'] + order[21:]), ('base_only', order[:16]), ('s03', order[:21])):
        p = os.path.join(tmp, 'synth_%s.csv' % name)
        keep = [order.index(c) if c in order else None for c in hdr]
        synth_csv(p, hdr, [[(r[k] if k is not None else '1') for k in keep] for r in rows])
        bench_line.read_frames(p)
    print('ok   F5 headers with a new base column, or stopping early, are read by name (not refused)')
    # F4: an unknown-CPU row leaves the mean unchanged
    n = len(rows)
    ri = order.index('fs_rest_ms')
    rows2 = [list(r) for r in rows]
    for i in (10, 11, 12):
        rows2[i][ri] = ''
    p2 = os.path.join(tmp, 'synth_rest_empty.csv')
    synth_csv(p2, order, rows2)
    sp = bench_line.window_numbers(bench_line.read_frames(p2), 1000, 1000 + n - 1)['split']['fs_rest_ms'][0]
    if abs(sp - 1.5) > 1e-9:
        raise Fail('F4: rows with an empty fs_rest_ms moved the mean to %s (expected 1.5, the known rows)' % sp)
    rows3 = [list(r) for r in rows2]
    for i in (10, 11, 12):
        rows3[i][ri] = '-1'
    p3 = os.path.join(tmp, 'synth_rest_minus1.csv')
    synth_csv(p3, order, rows3)
    sp3 = bench_line.window_numbers(bench_line.read_frames(p3), 1000, 1000 + n - 1)['split']['fs_rest_ms'][0]
    if abs(sp3 - 1.5) < 1e-6:
        raise Fail('F4 plant: -1 cells did NOT move the mean (the check cannot tell -1 from empty)')
    print('ok   F4 an unknown-CPU row (empty cell) leaves the mean unchanged (1.500); plant: -1 cells move it to %.3f' % sp3)
    # F7: the base branch's bench_line vs the new one on the same OLD-header csv
    base_commit = (vcs_out(root, 'log', '--format=%H', '--diff-filter=A', '-1', '--', 'tools/frame_split/frame_split_check.py') or '').strip()
    old_dir = None
    if base_commit and vcs_out(root, 'rev-parse', base_commit + '^'):
        d = os.path.join(tmp, 'base_bench_line')
        os.makedirs(d, exist_ok=True)
        okf = True
        for f in ('bench_line.py', 'heat_rules.py'):
            t = vcs_out(root, 'show', '%s^:tools/surface/%s' % (base_commit, f))
            if t is None:
                okf = False
                break
            with open(os.path.join(d, f), 'w', encoding='utf-8', newline='') as fh:
                fh.write(t)
        old_dir = d if okf else None
    if old_dir is None:
        print('skip F7 old-vs-new bench_line: the base branch\'s script is not reachable from this checkout\'s history (the synthetic fixtures above are checked against planted numbers)')
    else:
        import importlib.util
        spec = importlib.util.spec_from_file_location('bench_line_base', os.path.join(old_dir, 'bench_line.py'))
        sys.path.insert(0, old_dir)
        try:
            mod = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mod)
        finally:
            sys.path.remove(old_dir)
        for name in ('base_only', 's03'):
            p = os.path.join(tmp, 'synth_%s.csv' % name)
            a = mod.window_numbers(mod.read_frames(p), 1000, 1000 + n - 1)
            b = bench_line.window_numbers(bench_line.read_frames(p), 1000, 1000 + n - 1)
            a.pop('split', None), b.pop('split', None), b.pop('split_tail_n', None)
            for k in [k for k in b if k not in a and b[k] is None]:  # S102 D1: a field a later lane added (R21-B 'record') that this old csv cannot fill is not a difference
                b.pop(k)
            if a != b:
                raise Fail('F7: the new bench_line reads the old-header csv %s differently from the base branch\'s: %s vs %s' % (name, a, b))
        print('ok   F7 the new bench_line gives the same numbers as the base branch\'s script (commit %s^) on old-header csvs' % base_commit[:8])
    # F5: every historic header literal of src/frame_stats.h is accepted
    commits = (vcs_out(root, 'log', '--format=%H', '-G', 'fputs\\("frame,t_seconds', '--', 'src/frame_stats.h') or '').split()
    seen, checked = set(), 0
    for c in commits:
        t = vcs_out(root, 'show', '%s:src/frame_stats.h' % c)
        if not t:
            continue
        try:
            h = literal_header(t)
        except Fail:
            continue
        if h in seen:
            continue
        seen.add(h)
        bench_line.check_header(h.split(','), 'frame_stats.h@' + c[:8])
        checked += 1
    if checked < 2:
        raise Fail('F5: only %d historic header literal(s) found in the history (need the log of src/frame_stats.h)' % checked)
    print('ok   F5 every historic header literal of src/frame_stats.h (%d distinct, from %d commits) is accepted' % (checked, len(commits)))


def lint_sources(root):
    """F2 / F3 / F6 as text lints on the shipped sources, each with a plant."""
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'split'))
    from read_unit import read_path, locate  # a split file is read with its parts expanded; a hit names its part file and line
    reader = read_path(os.path.join(root, 'src', 'renderer_adapter', 'reader.h'), crlf=True)
    stats = open(os.path.join(root, 'src', 'frame_stats.h'), encoding='utf-8', newline='').read()

    def reset_problems(text):
        lines = text.replace('\r\n', '\n').split('\n')
        probs, n = [], 0
        for i, ln in enumerate(lines):
            if re.match(r'\s*g_times = \{\};', ln):
                n += 1
                prev = next((lines[j] for j in range(i - 1, -1, -1) if lines[j].strip()), '')
                if 'g_split_ticks.Bank(g_times.draw, g_times.present)' not in prev:
                    probs.append('%s: `g_times = {};` is not preceded by g_split_ticks.Bank(...)' % locate(root, 'src/renderer_adapter/reader.h', i + 1))
        if n == 0:
            probs.append('reader.h: no `g_times = {};` found (the lint cannot read its input)')
        return probs
    p = reset_problems(reader)
    if p:
        raise Fail('F2 lint: ' + '; '.join(p))
    planted = re.sub(r'[^\n]*g_split_ticks\.Bank\([^\n]*\n', '', reader, count=1)
    if planted == reader or not reset_problems(planted):
        raise Fail('F2 plant: a reset without the bank was NOT flagged')
    print('ok   F2 lint: every `g_times = {};` in reader.h is preceded by the running-total bank; plant (bank removed) flagged')

    def on_problems(text):
        t = text.replace('\r\n', '\n')
        stores = [m.start() for m in re.finditer(r'frame_split::g_on\.store\(', t)]
        probs = []
        if len(stores) != 1:
            probs.append('frame_split::g_on.store( appears %d times in frame_stats.h (must be exactly once: in OpenCsv)' % len(stores))
        elif 'split_csv_ = REXCVAR_GET(wwe2k14_frame_split);' not in t[max(0, stores[0] - 400):stores[0]]:
            probs.append('the g_on.store is not right after split_csv_ = REXCVAR_GET(...) in OpenCsv')
        if len(re.findall(r'REXCVAR_GET\(wwe2k14_frame_split\)', t)) != 1:
            probs.append('the cvar is read more than once in frame_stats.h (must be once, in OpenCsv)')
        return probs
    p = on_problems(stats)
    if p:
        raise Fail('F6 lint: ' + '; '.join(p))
    anchor = 'const uint64_t frame = ++g_guest_frame;'
    if anchor not in stats:  # S102 D1: K3 (review F6) changed the line to a load + 1; the plant's anchor follows it (the plant did not apply on s102-arc-pt2 a9c0e3ff)
        anchor = 'const uint64_t frame = g_guest_frame.load(std::memory_order_relaxed) + 1;'
    planted = stats.replace(anchor, anchor + ' frame_split::g_on.store(REXCVAR_GET(wwe2k14_frame_split), std::memory_order_relaxed);', 1)
    if planted == stats or not on_problems(planted):
        raise Fail('F6 plant: a per-frame cvar read of g_on (toggling mid-run) was NOT flagged')
    print('ok   F6 lint: g_on is set once in OpenCsv from split_csv_, the cvar is read once; plant (a per-frame store) flagged')
    # F3: no source or doc may claim "waits are not CPU"
    bad = []
    for rel in ('docs/renderer/FRAMES_CSV_SPLIT.md', 'src/frame_split_math.h', 'src/frame_split.h', 'tools/surface/bench_line.py', 'tools/frame_split/frame_split_test.cpp'):
        t = open(os.path.join(root, rel), encoding='utf-8', errors='replace').read().lower()
        if re.search(r'waits? (are|is) not cpu', t):
            bad.append(rel)
    if bad:
        raise Fail('F3 lint: the claim "waits are not CPU" is in ' + ', '.join(bad) + ' (the fence wait spins: it is CPU; rest = CPU minus wall-time parts, can be negative)')
    doc = open(os.path.join(root, 'docs', 'renderer', 'FRAMES_CSV_SPLIT.md'), encoding='utf-8').read()
    if 'can be negative' not in doc or 'CPU minus the wall-time parts' not in doc:
        raise Fail('F3 lint: FRAMES_CSV_SPLIT.md must define fs_rest_ms as "thread CPU minus the wall-time parts, can be negative"')
    print('ok   F3 lint: no "waits are not CPU" claim; the doc defines rest as CPU minus the wall-time parts, can be negative')


def part4(root, bench_line, order):
    main = main_checkout(root)
    # The old csv: R14's folder while it still has its frames csv (its numbers are known); out\ is tidied (S102: the csvs of ab5 were gone after the 18:34 reboot),
    # so an older bench folder stands in (exit 0 and 'not measured' only).
    old, old_known = None, False
    for cand, known in ((OLD_X + os.sep + 'frames_bryan', True), (os.path.join('out', 's101', 'surface_bench', 'y', 'ab_1_A'), False)):
        d = os.path.join(main or '', cand)
        if main and os.path.isdir(d) and any(f.startswith('frames_') and f.endswith('.csv') for _, _, fs in os.walk(d) for f in fs):
            old, old_known = d, known
            break
    if old is None:  # (F7: out\\ is tidied; the synthetic old-header fixtures below are the input that always exists, this real folder is an extra)
        print('skip a real old bench folder: none with a frames csv under the main checkout\'s out\\ (the synthetic old-header fixtures below stand in)')
    else:
        r = subprocess.run([sys.executable, '-I', os.path.join(root, 'tools', 'surface', 'bench_line.py'), os.path.dirname(old) if old_known else old, '--split', '--allow-no-heat'], capture_output=True, text=True)
        if r.returncode != 0 or 'not measured' not in r.stdout or (old_known and 'shown 59.6 fps, 1% low 44.7' not in r.stdout):
            raise Fail('bench_line on the OLD csv (%s): exit %d\n%s%s' % (old, r.returncode, r.stdout[:400], r.stderr[:400]))
        print('ok   bench_line reads the real old csv (%s) unchanged%s and says "split: not measured"' % (os.path.relpath(old, main), ' (bryan shown 59.6 fps, 1% low 44.7)' if old_known else ''))
    tmp = os.path.join(root, 'out', 'frame_split')
    os.makedirs(tmp, exist_ok=True)
    # synthetic csv: 100 frames, gap 16 ms except two tail frames of 30 ms that carry draw 20 / replay 9; split columns are constants elsewhere
    n = 100
    rows = []
    for i in range(n):
        tail = i in (40, 41)
        vals = {c: '' for c in order}
        vals.update({'frame': str(1000 + i), 't_seconds': '%.3f' % (i * 0.016), 'frame_ms': '30.0' if tail else '16.0', 'presented': '1', 'present_gap_ms': '30.0' if tail else '16.0',
                     'fs_reader_draw_ms': '20.0' if tail else '8.0', 'fs_reader_present_ms': '1.0', 'fs_scene_end_ms': '3.0', 'fs_replay_ms': '9.0' if tail else '4.0',
                     'fs_fence_wait_ms': '0.5', 'fs_barrier_wait_ms': '2.0', 'fs_rest_ms': '1.5'})
        rows.append([vals[c] for c in order])
    good = os.path.join(tmp, 'synth_good.csv')
    synth_csv(good, order, rows)
    got = bench_line.read_frames(good)
    num = bench_line.window_numbers(got, 1000, 1000 + n - 1)
    sp = num['split']
    exp_mean_draw = (98 * 8.0 + 2 * 20.0) / 100
    ok = (sp is not None and abs(sp['fs_reader_draw_ms'][0] - exp_mean_draw) < 1e-9 and abs(sp['fs_reader_draw_ms'][1] - 20.0) < 1e-9 and abs(sp['fs_replay_ms'][1] - 9.0) < 1e-9
          and abs(sp['fs_barrier_wait_ms'][0] - 2.0) < 1e-9 and num['split_tail_n'] == 2)
    if not ok:
        raise Fail('bench_line split numbers on the synthetic csv: %s (tail n %s)' % (sp, num.get('split_tail_n')))
    print('ok   bench_line split numbers: mean draw %.2f, 1%%-low-frame mean 20.00, tail frames %d' % (exp_mean_draw, num['split_tail_n']))
    # a csv cut short (an old run) is fine; a split column in front is refused; an unknown column is refused
    short = os.path.join(tmp, 'synth_short.csv')
    synth_csv(short, order[:16], [r[:16] for r in rows])
    bench_line.read_frames(short)
    bad_front = os.path.join(tmp, 'synth_front.csv')
    hdr = order[:2] + ['fs_rest_ms'] + order[2:21] + order[21:27]
    synth_csv(bad_front, hdr, [[''] * len(hdr)])
    try:
        bench_line.read_frames(bad_front)
    except bench_line.Refused as e:
        print('ok   plant 5 caught: a split column before an existing one is REFUSED (%s)' % str(e)[:90])
    else:
        raise Fail('plant 5: bench_line read a csv with a split column in front of an existing one')
    synth_extra_checks(root, bench_line, order, tmp, rows)
    bad_name = os.path.join(tmp, 'synth_name.csv')
    hdr2 = order[:21] + ['fs_mystery_ms'] + order[22:]
    synth_csv(bad_name, hdr2, [[''] * len(hdr2)])
    try:
        bench_line.read_frames(bad_name)
    except bench_line.Refused:
        print('ok   an unknown fs_ name is REFUSED')
    else:
        raise Fail('bench_line read a csv with an unknown column in a known position')
    # ab_line --split on an old folder prints 'not measured' per arm
    ab = os.path.join(main, 'out', 's102', 'ab6', 'abdm_bryan')
    if os.path.isdir(ab) and any(f.startswith('frames_') and f.endswith('.csv') for _, _, fs in os.walk(ab) for f in fs):  # (tidied folders keep no csv: skipped, said so)
        r = subprocess.run([sys.executable, '-I', os.path.join(root, 'tools', 'surface', 'ab_line.py'), ab, '--split'], capture_output=True, text=True)
        if r.returncode != 0 or 'split: not measured' not in r.stdout:
            raise Fail('ab_line --split on an old folder: exit %d\n%s' % (r.returncode, r.stdout[:300] + r.stderr[:300]))
        print('ok   ab_line --split reads an old ab folder (not measured per arm)')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    if not os.path.isfile(CLANG):
        print('FAIL: %s not found' % CLANG)
        return 1
    try:
        part1(root)
        plants(root)
        bench_line, order = part3(root)
        lint_sources(root)
        part4(root, bench_line, order)
    except Fail as e:
        print('FAIL: %s' % e)
        return 1
    print('PASS: the split columns: arithmetic, hooks, layout and parsers; 13 planted twins caught')
    return 0


if __name__ == '__main__':
    sys.exit(main())
