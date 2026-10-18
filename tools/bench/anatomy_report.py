"""Frame anatomy report: what a captured frame draws and where the command-processor (CP) time goes.

Input: logs/anatomy_NNN_K.csv from a frame-anatomy capture (SDK setting gpu_anatomy_capture=N, set mid-run with
the input-script command `cvar gpu_anatomy_capture N`; one row per IssueDraw/IssueCopy, columns in the file
header). Prints numbers only (shader hashes, register values, counts, times), never game data.

    py tools/bench/anatomy_report.py <anatomy_NNN_K.csv> [--top 25] [--frame F] [--gpu [anatomy_NNN_K_gpu.csv]]

Sections:
  totals      draws, copies, CP time (sum of cp_us) and per-draw cost per captured frame
  passes      consecutive draws grouped by render-target configuration (RB_SURFACE_INFO + color/depth info):
              one line per pass in submission order (draw count, CP ms inside IssueDraw, gap ms = CP time
              between commands (packet parsing, register writes; waits excluded only if none happen inside a
              frame), distinct shaders, resolves after)
  shaders     top shader pairs (vs_hash, ps_hash) by CP time: draws, CP ms, mean us/draw, primitive, counts
  skipped     draws that drew nothing, by reason code (see the SDK's AnatomySkipReason comment)
  pipelines   draws that changed the bound pipeline
  causes      (Session 7c, CAUSES; captures with the cause columns) what the CP recorded per row: uploads,
              texture loads, dispatches, barrier batches and their stages, rendering restarts, FSI EDRAM barriers,
              descriptor sets, pipeline creations - per frame totals and how many rows had each

With --gpu (Session 7b, GPUMARKS; the capture ran with gpu_anatomy_gpu_marks=true, which writes
anatomy_NNN_K_gpu.csv and anatomy_NNN_K_gpu_frames.csv next to the anatomy CSV; --gpu alone finds them), the GPU
timestamps are joined on (frame, idx). Per row: prep = GPU time from the previous mark of the same submission to
the mark right before the draw call (the draw's uploads, texture loads, barriers, render pass changes, plus
whatever ran before them in that stretch; for a submission's first row, from its start or, when later, from the
end of the previous submission's work); draw = the draw call itself (copies: the whole resolve). Sections:
  gpu totals   per frame: submissions, sum of command buffer times, their union (busy) and the idle gaps between
               them, draw prep / draw / resolve (prep + resolve) / block / submission-tail GPU ms, rows that drew
               but are in no GPU interval (frames left unmarked are listed as such, not counted there)
  gpu passes   the passes above (same frame) with GPU prep, draw, resolve and block ms
  gpu shaders  top shader pairs by GPU time (prep + draw + block share) per frame
  gpu prep     prep vs draw: prep per draw distribution, and the draws with the most prep in the frame
  gpu causes   (Session 7c, CAUSES; needs the cause columns) GPU prep by cause category of the rows the prep
               interval covers: uploads / texture loads / other dispatches / rendering restart with barriers /
               rendering restart without / barrier batches only / pipeline change only / none; single-row
               intervals and multi-row intervals (blocks) apart; barrier source stages with / without FRAGMENT
  gpu blocks   (sparse marks, gpu_anatomy_gpu_marks_mode=1, or any interval covering several rows) block,
               prep_begin, multi-row prep and tail intervals: totals, and block time attributed to the rows it
               covers (split evenly - an approximation) by shader pair
  gpu stalls   the longest intervals (preps and blocks) with the cause columns of the rows they cover
Intervals: the _gpu.csv's rows_covered / first_idx say which anatomy rows a row's interval (draw / copy: its prep;
block, prep_begin, cb_end: the whole interval) overlaps. A row's cause columns are its preparation: a draw's end
right after its draw call, a copy's right before the resolve (the resolve's own commands are its exec, in no row),
and a new submission starts a new window (what was recorded before it is the earlier submission's tail, in no row).
Bottom-of-pipe marks between draws can stop the GPU from overlapping consecutive draws, so a marked frame may
take longer on the GPU than an unmarked one: compare busy with gpu_ts of unmarked frames (sparse marks reduce
that distortion).
"""
import argparse
import csv
import os
import statistics
from collections import Counter, defaultdict

SKIP = {0: 'drew', 1: 'zero pitch', 2: 'no effect (no raster, no memexport)', 3: 'no vertices', 4: 'pipeline compiling',
        5: 'other', 6: 'failed', 7: 'no-op skipped (gpu_skip_noop_draws)'}

# Session 7c, CAUSES: the anatomy CSV's cause columns (empty when the SDK didn't count).
CAUSE_INT = ['up_calls', 'up_pages', 'tex_loads', 'tex_loads_scaled', 'tex_load_kib', 'dispatches', 'barrier_batches',
             'rendering_restarts', 'fsi_edram_barriers', 'desc_writes', 'pipeline_created', 'pipeline_pending']
CAUSE_HEX = ['barrier_src_stages', 'barrier_dst_stages']
# VkPipelineStageFlagBits.
STAGES = [(0x1, 'TOP'), (0x2, 'IND'), (0x4, 'VI'), (0x8, 'VS'), (0x10, 'TCS'), (0x20, 'TES'), (0x40, 'GS'),
          (0x80, 'FS'), (0x100, 'EFT'), (0x200, 'LFT'), (0x400, 'CAO'), (0x800, 'CS'), (0x1000, 'XFER'),
          (0x2000, 'BOT'), (0x4000, 'HOST'), (0x8000, 'AG'), (0x10000, 'AC')]
FRAGMENT_STAGE = 0x80
CATEGORIES = ['uploads', 'texture loads', 'other dispatches', 'restart + barriers', 'restart, no barrier',
              'barrier batches only', 'pipeline change only', 'none']


def load(path):
    rows = list(csv.DictReader(open(path, newline='')))
    for r in rows:
        r['frame'] = int(r['frame'])
        r['idx'] = int(r['idx'])
        r['cp_us'] = float(r['cp_us'] or 0)
        r['t_us'] = float(r['t_us'] or 0)
        r['skipped'] = int(r['skipped'] or 0)
        r['count'] = int(r['count'] or 0)
        # Session 7c, CAUSES: None without the columns (older captures) or when the backend didn't count.
        c = None
        if r.get('up_calls') not in (None, ''):
            c = {k: int(r[k] or 0) for k in CAUSE_INT}
            for k in CAUSE_HEX:
                c[k] = int(r[k] or '0', 16)
        r['c'] = c
    return rows


def rt_key(r):
    return (r['rb_surface_info'], r['rb_color_info0'], r['rb_color_info1'], r['rb_color_info2'], r['rb_color_info3'],
            r['rb_depth_info'], r['rb_modecontrol'])


def num(v):
    return float(v) if v not in ('', None) else None


def stages(mask):
    names = [n for bit, n in STAGES if mask & bit]
    return '|'.join(names) if names else '-'


def cause_sum(rows):
    """The cause columns summed over rows (stage masks ORed); None if no row has them."""
    have = [r['c'] for r in rows if r.get('c')]
    if not have:
        return None
    out = {k: 0 for k in CAUSE_INT + CAUSE_HEX}
    for c in have:
        for k in CAUSE_INT:
            out[k] += c[k]
        for k in CAUSE_HEX:
            out[k] |= c[k]
    out['pipeline_changed'] = sum(1 for r in rows if r.get('pipeline_changed') == '1')
    return out


def category(c):
    if c is None:
        return None
    if c['up_calls']:
        return 'uploads'
    if c['tex_loads']:
        return 'texture loads'
    if c['dispatches']:
        return 'other dispatches'
    if c['rendering_restarts']:
        return 'restart + barriers' if c['barrier_batches'] else 'restart, no barrier'
    if c['barrier_batches']:
        return 'barrier batches only'
    if c['pipeline_changed'] or c['pipeline_created']:
        return 'pipeline change only'
    return 'none'


def load_gpu(path):
    """anatomy_NNN_K_gpu.csv -> ({(frame, idx): draw/copy row}, [cb_begin/cb_end rows], [block/prep_begin rows])."""
    rows, cb, blocks = {}, [], []
    for r in csv.DictReader(open(path, newline='')):
        cov = r.get('rows_covered')
        g = dict(frame=int(r['frame']), idx=int(r['idx']), kind=r['kind'], submission=int(r['submission']),
                 t=num(r['gpu_t_us']), prep=num(r['gpu_prep_us']), exec=num(r['gpu_exec_us']),
                 row=num(r['gpu_row_us']),
                 # Session 7c, CAUSES: the anatomy rows the interval overlaps (None: older file or no interval).
                 covered=int(cov) if cov not in (None, '') else None,
                 first=int(r['first_idx']) if r.get('first_idx') not in (None, '') else None)
        if g['kind'] in ('draw', 'copy'):
            rows[(g['frame'], g['idx'])] = g
        elif g['kind'] in ('block', 'prep_begin'):
            blocks.append(g)
        else:
            cb.append(g)
    return rows, cb, blocks


def load_gpu_frames(path):
    if not os.path.exists(path):
        return {}
    out = {}
    for r in csv.DictReader(open(path, newline='')):
        out[int(r['frame'])] = {k: float(v) for k, v in r.items() if k != 'frame' and v not in ('', None)}
    return out


def gpu_paths(anatomy_csv, gpu_arg):
    if gpu_arg and gpu_arg != 'auto':
        gpu = gpu_arg
    else:
        stem, ext = os.path.splitext(anatomy_csv)
        gpu = stem + '_gpu' + ext
    stem, ext = os.path.splitext(gpu)
    return gpu, stem + '_frames' + ext


def g_ms(values):
    return sum(v for v in values if v is not None) / 1000


def g_copy_us(g):
    """A copy row's GPU time: prep + resolve, each when present (a copy without its end mark still has its prep)."""
    return (g['prep'] or 0) + (g['exec'] or 0)


def q(values, p):
    return values[min(len(values) - 1, int(p * len(values)))]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('csv')
    ap.add_argument('--top', type=int, default=25)
    ap.add_argument('--frame', type=int, default=None, help='only this captured frame (default: all)')
    ap.add_argument('--gpu', nargs='?', const='auto', default=None,
                    help='join the GPU marks (gpu_anatomy_gpu_marks); optional path, default <csv>_gpu.csv')
    args = ap.parse_args()
    rows = load(args.csv)
    if args.frame is not None:
        rows = [r for r in rows if r['frame'] == args.frame]
    frames = sorted({r['frame'] for r in rows})
    draws = [r for r in rows if r['kind'] == 'draw']
    copies = [r for r in rows if r['kind'] == 'copy']
    nf = max(1, len(frames))
    print(f'{args.csv}: {len(frames)} frame(s), {len(draws)} draws, {len(copies)} copies')
    print('\n== totals per frame')
    for f in frames:
        fd = [r for r in draws if r['frame'] == f]
        fc = [r for r in copies if r['frame'] == f]
        cp = sum(r['cp_us'] for r in fd + fc) / 1000
        drew = sum(1 for r in fd if r['skipped'] == 0)
        span = max((r['t_us'] + r['cp_us'] for r in fd + fc), default=0) / 1000
        print(f'  frame {f}: {len(fd)} draws ({drew} drew), {len(fc)} copies, CP time in draws+copies {cp:.2f} ms '
              f'(last command ends at {span:.2f} ms), {1000 * cp / max(1, len(fd)):.1f} us per draw')

    print('\n== passes (consecutive draws with the same render-target configuration), first frame')
    f0 = frames[0] if frames else None
    passes = []
    prev_end = None
    for r in rows:
        if r['frame'] != f0:
            continue
        # Gap = CP time between the previous command's end and this one's start: packet parsing, register
        # writes (type-0), waits; charged to the pass of the command that follows it.
        gap = max(0.0, r['t_us'] - prev_end) if prev_end is not None else 0.0
        prev_end = r['t_us'] + r['cp_us']
        if r['kind'] == 'copy':
            if passes:
                passes[-1]['copies'] += 1
                passes[-1]['gap'] += gap
                passes[-1]['copy_rows'].append(r)
            continue
        k = rt_key(r)
        if not passes or passes[-1]['key'] != k or passes[-1]['copies']:
            passes.append(dict(key=k, draws=0, cp=0.0, gap=0.0, shaders=set(), copies=0, prims=Counter(), drew=0,
                               depth_only=0, rows=[], copy_rows=[]))
        p = passes[-1]
        p['gap'] += gap
        p['draws'] += 1
        p['cp'] += r['cp_us']
        p['shaders'].add((r['vs_hash'], r['ps_hash']))
        p['prims'][r['prim']] += 1
        p['drew'] += r['skipped'] == 0
        p['depth_only'] += r['ps_hash'] in ('0', '0000000000000000', '')
        p['rows'].append(r)
    print(f'  {"#":>3} {"draws":>5} {"drew":>5} {"CP ms":>6} {"gap ms":>6} {"us/draw":>7} {"shaders":>7} {"depth-only":>10} {"resolves after":>14}  surface_info color0 depth_info')
    for i, p in enumerate(passes):
        k = p['key']
        print(f'  {i:3d} {p["draws"]:5d} {p["drew"]:5d} {p["cp"] / 1000:6.2f} {p["gap"] / 1000:6.2f} {p["cp"] / max(1, p["draws"]):7.1f} '
              f'{len(p["shaders"]):7d} {p["depth_only"]:10d} {p["copies"]:14d}  {k[0]} {k[1]} {k[5]}')

    print(f'\n== top {args.top} shader pairs by CP time (all frames, per frame)')
    by = defaultdict(list)
    for r in draws:
        by[(r['vs_hash'], r['ps_hash'])].append(r)
    total_cp = sum(r['cp_us'] for r in draws) or 1
    ranked = sorted(by.items(), key=lambda kv: -sum(r['cp_us'] for r in kv[1]))
    print(f'  {"vs_hash":>16} {"ps_hash":>16} {"draws/f":>7} {"CP ms/f":>7} {"share":>6} {"us/draw":>7} {"prim":>5} {"count p50":>9} {"vs/ps consts":>12} {"tex":>9}')
    for (vs, ps), rs in ranked[:args.top]:
        cp = sum(r['cp_us'] for r in rs)
        prims = Counter(r['prim'] for r in rs).most_common(1)[0][0]
        cnt = statistics.median(r['count'] for r in rs)
        print(f'  {vs:>16} {ps:>16} {len(rs) / nf:7.1f} {cp / 1000 / nf:7.2f} {100 * cp / total_cp:5.1f}% '
              f'{cp / len(rs):7.1f} {prims:>5} {cnt:9.0f} {rs[0]["vs_consts"]:>5}/{rs[0]["ps_consts"]:<6} '
              f'{rs[0]["vs_tex"]}/{rs[0]["ps_tex"]}')
    print(f'  ({len(by)} distinct shader pairs)')

    print('\n== skipped draws by reason (all frames)')
    for code, n in sorted(Counter(r['skipped'] for r in draws).items()):
        print(f'  {code} {SKIP.get(code, "?"):40s} {n / nf:8.1f} per frame')
    pc = sum(1 for r in draws if r['pipeline_changed'] == '1')
    print(f'\n== pipeline changes: {pc / nf:.1f} per frame ({100 * pc / max(1, len(draws)):.1f}% of draws)')
    # CP time distribution per draw
    cps = sorted(r['cp_us'] for r in draws)
    if cps:
        print(f'== CP us per draw: p50 {q(cps, 0.5):.1f} p90 {q(cps, 0.9):.1f} p99 {q(cps, 0.99):.1f} max {cps[-1]:.1f}; '
              f'top 1% of draws = {100 * sum(cps[int(0.99 * len(cps)):]) / total_cp:.1f}% of draw CP time')

    causes_report(rows, draws, copies, nf)

    if args.gpu is not None:
        gpu_report(args, rows, draws, copies, frames, passes, f0)


def causes_report(rows, draws, copies, nf):
    """Session 7c, CAUSES: per-frame totals of the cause columns, and how many rows had each."""
    have = [r for r in rows if r['c']]
    if not have:
        print('\n== causes: no cause columns in this capture (needs the Session 7c CAUSES SDK)')
        return
    print(f'\n== causes (what the CP recorded per row; per frame, {len(have)} of {len(rows)} rows counted)')
    print(f'  {"column":>20} {"draws: sum/f":>12} {"rows/f":>7} {"copies: sum/f":>13} {"rows/f":>7}')
    for k in CAUSE_INT:
        ds = sum(r['c'][k] for r in draws if r['c'])
        dn = sum(1 for r in draws if r['c'] and r['c'][k])
        cs = sum(r['c'][k] for r in copies if r['c'])
        cn = sum(1 for r in copies if r['c'] and r['c'][k])
        print(f'  {k:>20} {ds / nf:12.1f} {dn / nf:7.1f} {cs / nf:13.1f} {cn / nf:7.1f}')
    print('  (each row\'s preparation: a draw\'s ends right after its draw call, a copy\'s right before the resolve - '
          'the resolve\'s own commands, submission tails and the swap are in no row, only in cpf_sync / cpf_sync2)')
    cats = Counter(category(cause_sum([r])) for r in draws if r['c'] and r['skipped'] == 0)
    print('  draws that drew, by category of their own columns: ' +
          ', '.join(f'{c} {cats[c] / nf:.1f}' for c in CATEGORIES if cats[c]))
    src = Counter()
    for r in have:
        if r['c']['barrier_batches']:
            src[(stages(r['c']['barrier_src_stages']), stages(r['c']['barrier_dst_stages']))] += 1
    if src:
        print('  rows with barrier batches by stage masks (src -> dst, ORed per row), per frame:')
        for (s, d), n in src.most_common(8):
            print(f'    {n / nf:7.1f}  {s} -> {d}')


def gpu_report(args, rows, draws, copies, frames, passes, f0):
    """Session 7b, GPUMARKS: the GPU marks joined on (frame, idx)."""
    gpu_path, gpu_frames_path = gpu_paths(args.csv, args.gpu)
    if not os.path.exists(gpu_path):
        print(f'\n== GPU: {gpu_path} not found (capture without gpu_anatomy_gpu_marks?)')
        return
    gpu, cb, blocks = load_gpu(gpu_path)
    gframes = load_gpu_frames(gpu_frames_path)
    if args.frame is not None:
        cb = [c for c in cb if c['frame'] == args.frame]
        blocks = [b for b in blocks if b['frame'] == args.frame]
    # Join: the anatomy row's GPU data (None when the row has no marks, for example a draw that drew nothing).
    mismatched = 0
    for r in rows:
        g = gpu.get((r['frame'], r['idx']))
        if g is not None and g['kind'] != r['kind']:
            mismatched += 1
            g = None
        r['g'] = g
    by_key = {(r['frame'], r['idx']): r for r in rows}
    gframes_sel = [f for f in frames if f in gframes]
    frame_set = set(frames)
    # Frames with GPU marks (a frame is left unmarked when its query slot is still busy or the marks were turned
    # off mid-capture): in the _gpu_frames.csv, or with rows in the _gpu.csv.
    marked = set(gframes) | {g_frame for (g_frame, _) in gpu} | {c['frame'] for c in cb} | {b['frame'] for b in blocks}
    sparse_frames = {f for f, gf in gframes.items() if gf.get('marks_mode', 0) == 1}
    # Per-frame averages over the frames that have GPU marks.
    nfg = max(1, len(marked & frame_set))
    print(f'\n== GPU ({gpu_path}): {sum(1 for r in rows if r["g"])} of {len(rows)} rows have GPU marks'
          f'{f", {mismatched} kind mismatches (ignored)" if mismatched else ""}; {len(gframes_sel)} frame(s) in '
          f'{os.path.basename(gpu_frames_path)}'
          f'{f"; {len(sparse_frames & frame_set)} frame(s) with sparse marks (mode 1)" if sparse_frames else ""}')

    # Session 7c, CAUSES: the intervals and the anatomy rows they cover. kind: draw/copy (their prep), block,
    # prep_begin, tail (cb_end). Rows: the covered anatomy rows that exist in this capture.
    has_cov = any(g['covered'] is not None for g in gpu.values()) or any(b['covered'] is not None for b in blocks)

    def covered_rows(g):
        if g['covered'] is None or g['first'] is None:
            return [by_key[(g['frame'], g['idx'])]] if (g['frame'], g['idx']) in by_key and g['kind'] in (
                'draw', 'copy') else []
        return [by_key[(g['frame'], i)] for i in range(g['first'], g['first'] + g['covered'])
                if (g['frame'], i) in by_key]

    intervals = []
    for g in gpu.values():
        if args.frame is not None and g['frame'] != args.frame:
            continue
        if g['prep'] is not None:
            intervals.append(dict(kind=g['kind'], g=g, us=g['prep'], covered=g['covered'] if g['covered'] is not None
                                  else 1, rows=covered_rows(g)))
    for b in blocks:
        if b['row'] is not None:
            intervals.append(dict(kind=b['kind'], g=b, us=b['row'], covered=b['covered'] or 0, rows=covered_rows(b)))
    for c in cb:
        if c['kind'] == 'cb_end' and c['prep'] is not None:
            intervals.append(dict(kind='tail', g=c, us=c['prep'], covered=c['covered'] or 0, rows=covered_rows(c)))
    # Block time attributed to the rows a block covers (split evenly): blocks, prep_begin and tails; draw / copy
    # preps stay with their own row (as in the sections before Session 7c).
    block_share = defaultdict(float)
    for iv in intervals:
        if iv['kind'] in ('block', 'prep_begin', 'tail') and iv['rows']:
            share = iv['us'] / len(iv['rows'])
            for r in iv['rows']:
                block_share[(r['frame'], r['idx'])] += share
    in_interval = {(r['frame'], r['idx']) for iv in intervals for r in iv['rows']} if has_cov else set()

    print('\n== GPU totals per frame (ms)')
    print(f'  {"frame":>5} {"subm":>4} {"cb sum":>7} {"busy":>7} {"idle":>6} {"draw prep":>9} {"draw":>7} '
          f'{"resolve":>7} {"blocks":>6} {"tails":>6} {"sum":>7} {"no GPU":>6} {"marks":>6} {"dropped":>7} '
          f'{"no result":>9} {"mode":>4}')
    unmarked = []
    for f in frames:
        if f not in marked:
            unmarked.append(f)
            continue
        fd = [r for r in draws if r['frame'] == f]
        fc = [r for r in copies if r['frame'] == f]
        prep = g_ms(r['g']['prep'] for r in fd if r['g'])
        exe = g_ms(r['g']['exec'] for r in fd if r['g'])
        res = g_ms(g_copy_us(r['g']) for r in fc if r['g'])
        blk = g_ms(b['row'] for b in blocks if b['frame'] == f)
        tails = g_ms(c['prep'] for c in cb if c['frame'] == f and c['kind'] == 'cb_end')
        missing = sum(1 for r in fd + fc if not r['g'] and r['skipped'] == 0
                      and (r['frame'], r['idx']) not in in_interval)
        gf = gframes.get(f, {})
        print(f'  {f:5d} {gf.get("submissions", 0):4.0f} {gf.get("cb_sum_us", 0) / 1000:7.2f} '
              f'{gf.get("busy_union_us", 0) / 1000:7.2f} {gf.get("idle_gap_us", 0) / 1000:6.2f} {prep:9.2f} {exe:7.2f} '
              f'{res:7.2f} {blk:6.2f} {tails:6.2f} {prep + exe + res + blk + tails:7.2f} {missing:6d} '
              f'{gf.get("marks", 0):6.0f} {gf.get("marks_dropped", 0):7.0f} {gf.get("marks_unavailable", 0):9.0f} '
              f'{gf.get("marks_mode", 0):4.0f}')
    if unmarked:
        shown = ' '.join(str(f) for f in unmarked[:20]) + (' ...' if len(unmarked) > 20 else '')
        print(f'  {len(unmarked)} frame(s) without GPU marks (query slot busy or marks off): {shown}')
    print('  (each submission\'s marks chain from cb_begin to cb_end, so sum ~= cb sum when no mark is missing; '
          '"no GPU" = rows that drew but are in no GPU interval, in marked frames; mode 1 = sparse marks: unmarked '
          'draws are inside block intervals)')

    print(f'\n== GPU per pass (frame {f0}; same passes as above, ms; block = block time split over the rows it '
          f'covers){"" if f0 in marked else " - frame not marked, no GPU data"}')
    print(f'  {"#":>3} {"draws":>5} {"prep":>6} {"draw":>6} {"resolve":>7} {"block":>6} {"total":>6} {"share":>6} '
          f'{"draw us/d":>9} {"prep us/d":>9}  surface_info color0 depth_info')
    totals = []
    for p in passes:
        prep = g_ms(r['g']['prep'] for r in p['rows'] if r['g'])
        exe = g_ms(r['g']['exec'] for r in p['rows'] if r['g'])
        res = g_ms(g_copy_us(r['g']) for r in p['copy_rows'] if r['g'])
        blk = sum(block_share.get((r['frame'], r['idx']), 0.0) for r in p['rows'] + p['copy_rows']) / 1000
        totals.append((prep, exe, res, blk))
    frame_total = sum(sum(t) for t in totals) or 1
    for i, (p, (prep, exe, res, blk)) in enumerate(zip(passes, totals)):
        k = p['key']
        nd = max(1, sum(1 for r in p['rows'] if r['g']))
        print(f'  {i:3d} {p["draws"]:5d} {prep:6.2f} {exe:6.2f} {res:7.2f} {blk:6.2f} {prep + exe + res + blk:6.2f} '
              f'{100 * (prep + exe + res + blk) / frame_total:5.1f}% {1000 * exe / nd:9.1f} {1000 * prep / nd:9.1f}  '
              f'{k[0]} {k[1]} {k[5]}')

    print(f'\n== top {args.top} shader pairs by GPU time (prep + draw + block share; all frames, per frame)')
    by = defaultdict(list)
    for r in draws:
        if r['g'] or (r['frame'], r['idx']) in block_share:
            by[(r['vs_hash'], r['ps_hash'])].append(r)

    def pair_us(r):
        g = r['g']
        own = ((g['prep'] or 0) + (g['exec'] or 0)) if g else 0
        return own + block_share.get((r['frame'], r['idx']), 0.0)

    gtotal = sum(pair_us(r) for rs in by.values() for r in rs) or 1
    ranked = sorted(by.items(), key=lambda kv: -sum(pair_us(r) for r in kv[1]))
    print(f'  {"vs_hash":>16} {"ps_hash":>16} {"draws/f":>7} {"GPU ms/f":>8} {"share":>6} {"draw ms/f":>9} '
          f'{"prep ms/f":>9} {"block ms/f":>10} {"draw us p50":>11} {"draw us max":>11} {"CP ms/f":>7}')
    for (vs, ps), rs in ranked[:args.top]:
        exe = [r['g']['exec'] for r in rs if r['g'] and r['g']['exec'] is not None]
        prep = [r['g']['prep'] for r in rs if r['g'] and r['g']['prep'] is not None]
        blk = sum(block_share.get((r['frame'], r['idx']), 0.0) for r in rs)
        tot = sum(exe) + sum(prep) + blk
        print(f'  {vs:>16} {ps:>16} {len(rs) / nfg:7.1f} {tot / 1000 / nfg:8.2f} {100 * tot / gtotal:5.1f}% '
              f'{sum(exe) / 1000 / nfg:9.2f} {sum(prep) / 1000 / nfg:9.2f} {blk / 1000 / nfg:10.2f} '
              f'{statistics.median(exe) if exe else 0:11.1f} {max(exe) if exe else 0:11.1f} '
              f'{sum(r["cp_us"] for r in rs) / 1000 / nfg:7.2f}')
    print(f'  ({len(by)} distinct shader pairs with GPU data)')

    print('\n== GPU prep (uploads, texture loads, barriers, render pass changes before each draw) vs draw')
    if sparse_frames:
        print('  (sparse marks: only marked draws have prep / draw values - the eventful ones and the resolves)')
    preps = sorted(r['g']['prep'] for r in draws if r['g'] and r['g']['prep'] is not None)
    exes = sorted(r['g']['exec'] for r in draws if r['g'] and r['g']['exec'] is not None)
    if preps and exes:
        sp, se = sum(preps), sum(exes)
        print(f'  per frame: draw prep {sp / 1000 / nfg:.2f} ms, draw {se / 1000 / nfg:.2f} ms '
              f'(prep = {100 * sp / max(1e-9, sp + se):.1f}% of draw rows)')
        print(f'  prep us per draw: p50 {q(preps, 0.5):.1f} p90 {q(preps, 0.9):.1f} p99 {q(preps, 0.99):.1f} '
              f'max {preps[-1]:.1f}; draws with prep > 5 us: {sum(1 for v in preps if v > 5) / nfg:.1f} per frame '
              f'= {100 * sum(v for v in preps if v > 5) / max(1e-9, sp):.1f}% of prep')
        print(f'  draw us per draw: p50 {q(exes, 0.5):.1f} p90 {q(exes, 0.9):.1f} p99 {q(exes, 0.99):.1f} '
              f'max {exes[-1]:.1f}')
        pc_prep = [r['g']['prep'] for r in draws if r['g'] and r['g']['prep'] is not None
                   and r['pipeline_changed'] == '1']
        print(f'  prep at draws that changed the pipeline: {g_ms(pc_prep) / nfg:.2f} ms per frame '
              f'({len(pc_prep) / nfg:.1f} draws)')
    fr = [r for r in draws if r['frame'] == f0 and r['g'] and r['g']['prep'] is not None]
    fr.sort(key=lambda r: -r['g']['prep'])
    print(f'  top {min(args.top, len(fr))} draws by prep in frame {f0}:')
    print(f'  {"idx":>5} {"prep us":>8} {"draw us":>8} {"t ms":>7} {"subm":>5} {"vs_hash":>16} {"ps_hash":>16} '
          f'{"pipe chg":>8}  surface_info')
    for r in fr[:args.top]:
        g = r['g']
        print(f'  {r["idx"]:5d} {g["prep"]:8.1f} {g["exec"] if g["exec"] is not None else float("nan"):8.1f} '
              f'{(g["t"] or 0) / 1000:7.2f} {g["submission"]:5d} {r["vs_hash"]:>16} {r["ps_hash"]:>16} '
              f'{r["pipeline_changed"]:>8}  {r["rb_surface_info"]}')

    gpu_causes_report(args, intervals, nfg, has_cov)


def interval_causes(iv):
    """The cause columns of the rows an interval covers (each row's columns are its preparation, a copy's included:
    the SDK ends a copy's window right before the resolve)."""
    rs = iv['rows']
    if not rs:
        # No row in the interval (a submission tail after the last row's end): submission-end commands only, which
        # no row counts.
        return None
    return cause_sum(rs)


def gpu_causes_report(args, intervals, nfg, has_cov):
    """Session 7c, CAUSES: GPU prep by cause category, blocks, and the top stalls with their cause columns."""
    if not intervals:
        return
    have_causes = any(r['c'] for iv in intervals for r in iv['rows'])
    for iv in intervals:
        iv['c'] = interval_causes(iv) if have_causes else None
        if not have_causes:
            iv['cat'] = None
        elif iv['c'] is None:
            iv['cat'] = 'no row'  # see interval_causes
        else:
            iv['cat'] = category(iv['c'])
        iv['single'] = iv['kind'] in ('draw', 'copy') and iv['covered'] <= 1

    if have_causes:
        print('\n== GPU causes: prep intervals by cause category of the rows they cover (per frame)')
        print('  single = a draw\'s / copy\'s prep covering only its own row; multi = intervals covering several rows '
              '(blocks, prep_begin, multi-row preps, submission tails), categorized by the union of their rows')
        print(f'  {"category":>22} {"single n/f":>10} {"ms/f":>7} {"share":>6} {"us mean":>8} {"us p90":>7} '
              f'{"us max":>7} {"> 50 us/f":>9} {"multi n/f":>9} {"ms/f":>7}')
        single = [iv for iv in intervals if iv['single'] and iv['kind'] == 'draw']
        multi = [iv for iv in intervals if not iv['single']]
        total_single = sum(iv['us'] for iv in single) or 1
        for cat in CATEGORIES + ['no row']:
            s = sorted(iv['us'] for iv in single if iv['cat'] == cat)
            m = [iv['us'] for iv in multi if iv['cat'] == cat]
            if not s and not m:
                continue
            name = cat if cat != 'no row' else 'no row (tails etc.)'
            print(f'  {name:>22} {len(s) / nfg:10.1f} {sum(s) / 1000 / nfg:7.2f} {100 * sum(s) / total_single:5.1f}% '
                  f'{(sum(s) / len(s)) if s else 0:8.1f} {q(s, 0.9) if s else 0:7.1f} {s[-1] if s else 0:7.1f} '
                  f'{sum(1 for v in s if v > 50) / nfg:9.1f} {len(m) / nfg:9.1f} {sum(m) / 1000 / nfg:7.2f}')
        cs = [iv for iv in intervals if iv['single'] and iv['kind'] == 'copy']
        ccat = defaultdict(list)
        for iv in cs:
            ccat[iv['cat']].append(iv['us'])
        by_cat = ', '.join(f'{c} {len(ccat[c]) / nfg:.1f}/f {sum(ccat[c]) / 1000 / nfg:.2f} ms'
                           for c in CATEGORIES + ['no row'] if ccat.get(c))
        print(f'  (draw rows only in "single" above; single-row copy preps: {len(cs) / nfg:.1f}/f, '
              f'{sum(iv["us"] for iv in cs) / 1000 / nfg:.2f} ms/f{": " + by_cat if by_cat else ""} - a resolve\'s own '
              f'work is its exec, in the gpu totals)')
        # Barrier stages: single-row draw preps with barrier batches, by whether FRAGMENT is in the source stages
        # (such a barrier waits for the in-flight pixel work, the FSI EDRAM emulation included).
        bar = [iv for iv in single if iv['c'] and iv['c']['barrier_batches']]
        if bar:
            fs = [iv['us'] for iv in bar if iv['c']['barrier_src_stages'] & FRAGMENT_STAGE]
            nofs = [iv['us'] for iv in bar if not iv['c']['barrier_src_stages'] & FRAGMENT_STAGE]
            print(f'  single-row draw preps with barrier batches: source stages with FS {len(fs) / nfg:.1f}/f '
                  f'{sum(fs) / 1000 / nfg:.2f} ms/f (mean {sum(fs) / max(1, len(fs)):.1f} us), without FS '
                  f'{len(nofs) / nfg:.1f}/f {sum(nofs) / 1000 / nfg:.2f} ms/f (mean {sum(nofs) / max(1, len(nofs)):.1f} us)')
            by_stage = defaultdict(list)
            for iv in bar:
                by_stage[(stages(iv['c']['barrier_src_stages']), stages(iv['c']['barrier_dst_stages']))].append(iv['us'])
            print('  by stage masks (src -> dst, ORed per row), top by GPU time:')
            for (s, d), us in sorted(by_stage.items(), key=lambda kv: -sum(kv[1]))[:8]:
                print(f'    {len(us) / nfg:7.1f}/f {sum(us) / 1000 / nfg:6.2f} ms/f  mean {sum(us) / len(us):6.1f} us  '
                      f'{s} -> {d}')

    multi_any = [iv for iv in intervals if not iv['single']]
    if multi_any and has_cov:
        print('\n== GPU blocks (intervals covering several rows; per frame)')
        print(f'  {"kind":>12} {"n/f":>7} {"ms/f":>7} {"rows/f":>8} {"us/row":>7} {"us max":>8}')
        for kind in ('block', 'prep_begin', 'draw', 'copy', 'tail'):
            ks = [iv for iv in multi_any if iv['kind'] == kind]
            if not ks:
                continue
            us = sum(iv['us'] for iv in ks)
            nrows = sum(iv['covered'] for iv in ks)
            label = kind if kind in ('block', 'prep_begin', 'tail') else f'{kind} prep'
            per_row = f'{us / nrows:7.2f}' if nrows else f'{"-":>7}'
            print(f'  {label:>12} {len(ks) / nfg:7.1f} {us / 1000 / nfg:7.2f} {nrows / nfg:8.1f} {per_row} '
                  f'{max(iv["us"] for iv in ks):8.1f}')
        print('  (block = after every gpu_anatomy_gpu_marks_every-th unmarked draw; prep_begin = right before the first '
              'event command after an unmarked draw; draw/copy prep = a marked row\'s prep that also holds earlier '
              'unmarked or skipped rows; tail = after the last mark to the end of the command buffer)')

    print(f'\n== GPU stalls: top {args.top} intervals by GPU time (preps and blocks) with the cause columns of the '
          f'rows they cover')
    print(f'  {"frame":>5} {"rows":>11} {"kind":>10} {"us":>7} {"cat":>20} {"up(pg)":>8} {"tex(sc,KiB)":>13} '
          f'{"disp":>4} {"bar":>3} {"rst":>3} {"fsi":>3} {"desc":>4} {"pipe c/n/p":>10}  barrier stages src -> dst  '
          f'vs_hash ps_hash (last row)')
    ranked = sorted(intervals, key=lambda iv: -iv['us'])
    for iv in ranked[:args.top]:
        g = iv['g']
        rs = iv['rows']
        span = f'{rs[0]["idx"]}-{rs[-1]["idx"]}' if len(rs) > 1 else (str(rs[0]['idx']) if rs else str(g['idx']))
        c = iv['c']
        last = rs[-1] if rs else None
        shaders = f'{last["vs_hash"]} {last["ps_hash"]}' if last and last['kind'] == 'draw' else (
            last['kind'] if last else '')
        if c:
            print(f'  {g["frame"]:5d} {span:>11} {iv["kind"]:>10} {iv["us"]:7.1f} {iv["cat"]:>20} '
                  f'{c["up_calls"]:>3}({c["up_pages"]:>3}) {c["tex_loads"]:>3}({c["tex_loads_scaled"]},{c["tex_load_kib"]:>4}) '
                  f'{c["dispatches"]:4d} {c["barrier_batches"]:3d} {c["rendering_restarts"]:3d} '
                  f'{c["fsi_edram_barriers"]:3d} {c["desc_writes"]:4d} '
                  f'{c["pipeline_changed"]:>2}/{c["pipeline_created"]}/{c["pipeline_pending"]:<4}  '
                  f'{stages(c["barrier_src_stages"])} -> {stages(c["barrier_dst_stages"])}  {shaders}')
        else:
            why = 'no row' if iv['cat'] == 'no row' else 'no cause columns'
            print(f'  {g["frame"]:5d} {span:>11} {iv["kind"]:>10} {iv["us"]:7.1f} {why:>20}  {shaders}')


if __name__ == '__main__':
    main()
