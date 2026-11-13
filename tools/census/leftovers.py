r"""What is still the stream's (S101 lane 58, RENDERER_ARC.md C6): the leftovers census.

With the promotions on (fview --crowd-ours --arena-ours --wrestlers-ours --ring-ours --flash-ours --particles-ours --ui-ours; lane 73 added the last four: --promote
crowd,arena,wrestlers,ring,flash,particles,ui | none | auto = everything the capture and the fview build allow) every draw of a captured frame is either drawn by one of OUR drawers
(fate crowd_ours / crowd_injected / arena_ours / wrestlers_ours / ring_ours / flash_ours / ui_ours / particle_run: the stream's draw is dropped or replaced in place) or still the game's own (fate stream). This tool counts
the second kind by class / technique (the pass) / call site, with the primitives and the share of the final picture each one owns, so the arc's to-do
list is measured, not guessed.

  py -I tools\census\leftovers.py CASE_ZIP_OR_CAPTURE_DIR [--frames 200,260 | last] [--promote crowd,arena,wrestlers | none | auto]
                                  [--grid 64x36] [--name NAME] [--out DIR] [--fview EXE] [--md FILE] [--plant NAME]

Input: a case zip (a capture folder at capture\) or a capture folder. Output (main checkout's out\s101\leftovers\<name>\ ; game-derived, local only):
leftovers.json (everything below), leftovers.md (the tables). Printed: the same tables.

Two fview processes per frame (the dumps are of one bound frame; the first n frames are replayed to reach it):
  1. --ops --json     every draw: fate, class, technique, call site, pass site, object/part, widget, primitives, viewport; the target changes (SetTargets)
  2. --judge-out + --judge-pixels "grid W H"   for a W x H grid of the final picture: the chain of draws behind each sample (tools\judge)
Pixel share = the share of grid samples whose OWNER is the draw. Owner = the newest draw in the pixel's chain that is not a full-screen pass and changed
the pixel (a full-screen pass is followed down to the surface it read; the walk is judge.cpp's). `touch` = the share of samples that at least one draw
of the row touched (blended layers too; a pixel counts once per row). A merged particle run is ONE draw (its first), the others are folded (fate particle_run); the run's pixels sit on its first op, which keeps fate stream in fview and is relabelled particle_run_head here (counted as ours). A sample whose chain holds no such draw (a copy, a clear, a post pass only) is `post/copy only`. Our crowd's pixels sit on the FIRST dropped crowd
draw (its ops carry the whole crowd), so a fate crowd_injected owner is counted as `ours`, never as leftover.

`--plant NAME` mangles the draw table before it is counted (the selftest's planted twins; each must break an independent identity):
  fate_swap   every crowd_ours / crowd_injected / arena_ours / wrestlers_ours draw is read as stream
  dup_draws   the draw table is read twice
  newfate_swap  (S101 lane 73) every ring_ours / flash_ours / ui_ours / ui_text_ours / shadow_casters_ours / props_ours / screens_ours / particle_run(_head) draw is read as stream
  thin_ignored  (lane 143) the old behaviour: a thin capture is replayed WITHOUT fview's --thin (the lite draws carry no geometry and are dropped); the last-frame proof bar must fail
  emu_shared_tables  (lane 149) fview's old thin emulation (one stamp / facts table for every loaded frame): the bind the picture comes from loses arena draws (J1: 47, the spotlight beams among them); the not-learned guard must mark the frame INCOMPLETE

Lane 143: THIN CAPTURES AND THE PROOF BAR (Jarrod's Check J1 finding: a capture made with arena / ring / wrestlers THIN on was redrawn without the ring and the arena and the census said "99.4 % ours").
  - A capture whose frames carry kFrameHasThin (frame header flags & 32; src\renderer_adapter\adapter_frame.h, THIN_DRAWS.md) is a THIN capture: its lite draws hold no geometry. The census then passes
    fview `--thin arena,ring,wrestlers` (the classes that are promoted) so the lite draws are drawn from templates learned from the capture's own full / proof frames (the frames before the one measured are replayed).
  - fview prints `[fview] thin frame F: L lite draws, N not learned`: N > 0 means draws of that frame were drawn by nothing (their key was not learned yet). The census marks such a frame INCOMPLETE
    (its numbers are not a measurement) and the run exits 3. Pick frames after learning, or accept the report.
  - THE PROOF BAR (every case zip, the general guard, any cause): the census's redraw of the capture's LAST frame must match the zip's picture.png (what the live game showed) within owned_check's
    tolerance (proof_bar.py: p99.9 <= 8, 16x16 block mean <= 2, share over 4 <= 0.3 %). Otherwise the run says FAIL (UNPROVEN) and exits 3; the numbers are still written, marked `unproven`.
    --no-proof-bar switches it off (said loudly in the output).
"""
import argparse
import collections
import json
import os
import subprocess
import sys
import time
import zipfile
import re
from pathlib import Path

HERE = Path(__file__).resolve().parents[2]

PROMOTED = ('caw_ours', 'crowd_ours', 'crowd_injected', 'arena_ours', 'wrestlers_ours', 'ring_ours', 'ui_ours', 'flash_ours', 'particle_run', 'particle_run_head', 'props_ours', 'ui_text_ours', 'shadow_casters_ours', 'screens_ours', 'post_ours', 'rim_ours', 'movie_ours', 'small_passes_ours', 'ui_quads_ours', 'rim_chain_ours', 'circles_ours', 'plane_shadow_ours', 'replaced')  # S101 lane 67: particle_run; lane 84: props_ours
# S101 lane 73: every promotion the census can switch on: name -> (fview flag, needs the capture's scene records, fates it produces)
PROMOTIONS = {
    'crowd': ('--crowd-ours', True), 'arena': ('--arena-ours', True), 'wrestlers': ('--wrestlers-ours', True),
    'caw': ('--caw-ours', False),  # lane 153 (N3): the Create a Superstar editor's wrestler, part by part from pac/edit; needs no scene record; the fate is wrestlers_ours
    'ring': ('--ring-ours', True), 'flash': ('--flash-ours', True),
    'floor': ('--crowd-floor', True),  # S101 lane 84 (coordinator): lane 82's seated floor people (site 0x8276ED60), only meaningful with crowd on; their draws carry the crowd_ours / crowd_injected fates
    'cards': ('--crowd-cards', True),  # S101 lane CD: the stands' far cards (site 0x825130C4) as one instanced draw per person type; only with crowd on; opt-in (auto does not add it: --promote auto,cards)
    'props': ('--props-ours', True),  # S101 lane 84: the commentary table, the ring steps, the announcer's microphone (after the ring and the flashes)
    'crowd_props': ('--crowd-props', True),  # S101 lane 133: the crowd's sign / foam-hand draws (call sites 0x82518D60 / 0x825192D0); fate crowd_ours; needs the record's arena
    'screens': ('--screens-ours', True),  # S101 lane 115: the stage screens (site 0x82C0F4BC): the ribbon band, the wall behind the stage, the mini-tron (geometry only)
    'shadow_casters': ('--shadow-casters-ours', True),  # S101 lane 99: the shadow passes' casters (wrestler maps, plane mask); its joins read the wrestlers' and the ring's, so they are listed before it
    'post': ('--post-ours', False),            # S101 lane PO: the post chain's draws dropped, our pass draws them (no scene record needed); fate post_ours. The pixels of our pass sit on the first dropped draw's op (the dof)
    'rim_map': ('--rim-ours', True),  # lane 124: the rim light map's casters (site 0x82BFBD58); its joins read the wrestlers'
    'small_passes': ('--small-passes-ours', False),  # lane 135: the sweat map (yLiquidMap) and skin pre-light (yPreLight) passes; its own call sites, needs no scene record
    'rim_chain': ('--rim-chain-ours', False),  # lane 151: the rim light map's reductions and blur (own programs); its own call sites, needs no scene record
    'particles': ('--particles-ours', False),  # needs the draw origins (names), not a scene record

    'movie': ('--movie-ours', False),          # S101 lane 131: the movie planes (T_SpriteYUVAlpha, call site 0x82BC7E50) by our quad and YUV shader; needs no scene record; fate movie_ours
    'ui_text': ('--ui-text-ours', False),      # S101 lane 125: lane 89's menu text laid out by us (fate ui_text_ours); needs no scene record (session.cpp:167)
    'ui': ('--ui-ours', False),                # S101 lane 125: needs no scene record either (session.cpp:167 exempts ui, ui_text, shadow_casters; lane 73's True was stale). UI contract: probed per run
}
ALL_PROMOTIONS = list(PROMOTIONS)


def main_checkout() -> Path:
    env = os.environ.get('RDR_MAIN_CHECKOUT')
    if env:
        return Path(env)
    try:
        common = subprocess.run(['git', '-C', str(HERE), 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True,
                                timeout=20).stdout.strip()
        if common:
            return Path(common).parent
    except Exception:
        pass
    return HERE


def fail(msg):
    sys.exit('FAIL ' + msg)


def run(cmd, timeout, what):
    t0 = time.time()
    try:
        p = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, timeout=timeout, errors='replace')
    except subprocess.TimeoutExpired:
        fail(f'{what}: timeout after {timeout}s')
    out = p.stdout + p.stderr
    # The GPU is shared: only the "frame ring" start failure is retried (common.md), a few times.
    for attempt in range(3):
        if p.returncode == 0 or 'frame ring' not in out:
            break
        print(f'[leftovers] {what}: "frame ring" error, retry {attempt + 1}', flush=True)
        time.sleep(15)
        p = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, timeout=timeout, errors='replace')
        out = p.stdout + p.stderr
    if p.returncode != 0:
        fail(f'{what}: exit {p.returncode}\n' + out[-1500:])
    return time.time() - t0, out


def records_hold_floor(cap: Path) -> bool:
    """S101 lane 84: do the capture's scene records hold floor members (objects named "crowd:floor:TT:member:I", lane 82)? Looks at up to 6 records spread over the capture."""
    recs = sorted((cap / 'scene').glob('frame_*.rsc')) if (cap / 'scene').is_dir() else []
    for r in [recs[i * (len(recs) - 1) // 5] for i in range(6)] if len(recs) > 1 else recs:
        try:
            if b'crowd:floor:' in r.read_bytes():
                return True
        except OSError:
            pass
    return False


def open_input(src: Path, work: Path) -> Path:
    """A capture folder: the folder itself, or the capture\\ of a case zip (extracted once into work)."""
    if src.is_dir():
        if (src / 'capture').is_dir():
            return src / 'capture'
        return src
    if src.suffix.lower() != '.zip':
        fail(f'{src}: neither a folder nor a zip')
    dest = work / 'case'
    if not (dest / 'capture' / 'images.bin').exists():
        dest.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(src) as z:
            z.extractall(dest)
    return dest / 'capture'


# ---------------------------------------------------------------- the tables
def load_draws(ops_json):
    """-> (draws, ops_total, targets): draws = one dict per draw op with the target (colour images + depth) in force; targets = target key -> info."""
    d = json.loads(Path(ops_json).read_text(encoding='utf-8'))
    draws, target = [], 'none'
    for o in d['ops']:
        if o['kind'] == 'targets':
            target = 'c' + '+'.join(str(c) for c in o.get('colors', [])) + '/d' + str(o.get('depth', 0))
        elif o['kind'] == 'draw':
            o = dict(o)
            o['target'] = target
            draws.append(o)
    return draws, d['op_count'], d.get('kinds', {})


NEW_FATES = ('caw_ours', 'ring_ours', 'ui_ours', 'flash_ours', 'particle_run', 'particle_run_head', 'props_ours', 'ui_text_ours', 'shadow_casters_ours', 'screens_ours', 'post_ours', 'rim_ours', 'movie_ours', 'small_passes_ours', 'ui_quads_ours', 'rim_chain_ours')  # lane 151: rim_chain_ours  # lane 148: ui_quads_ours  # lane 124: rim_ours  # S101 lane 84: props_ours; lane 89: ui_text_ours


RUN_VARIANTS = ('yParticleRuns_runs_vertex', 'ySoftParticleRuns_runs_vertex')  # S101 lane 132


def mark_particle_heads(draws):
    """S101 lane 73: fview marks only the FOLDED members of a merged particle run (fate particle_run); the run's first draw keeps fate stream because it is the one draw that
    carries the whole run (binder.h ParticleEnd). It is ours too: a stream-fate draw with the same call site and technique, directly followed (in draw order) by particle_run draws is relabelled
    `particle_run_head`. A run of one (nothing folded) stays stream. The pixel share credits the whole run's pixels to this head (the first op), as the run is one draw."""
    out = list(draws)
    for i, d in enumerate(out):  # S101 lane 132 (review 2): a stream-fate draw whose variant is one of OUR run vertex stages carried a merged run (a run of one included): it is ours
        if d['fate'] == 'stream' and d.get('vs_variant') in RUN_VARIANTS:
            out[i] = dict(d, fate='particle_run_head')
    for i in range(len(out) - 1):
        d, nxt = out[i], out[i + 1]
        if d['fate'] == 'stream' and nxt['fate'] == 'particle_run' and d['call_site'] == nxt['call_site'] and d['technique'] == nxt['technique']:  # (Bryan's runs are class 'other': the class is not tested)
            out[i] = dict(d, fate='particle_run_head')
    return out


def plant_draws(draws, plant):
    if not plant:
        return draws
    if plant == 'fate_swap':
        return [dict(d, fate='stream') if d['fate'] in PROMOTED else d for d in draws]
    if plant == 'dup_draws':
        return draws + draws
    if plant == 'newfate_swap':  # S101 lane 73: only the fates added after lane 58 (ring / flash / ui / particle runs) are read as stream
        return [dict(d, fate='stream') if d['fate'] in NEW_FATES else d for d in draws]
    if plant == 'full_screen_by_count':  # S101 lane CJ: acts in owners(), not on the draw table
        return draws
    if plant == 'thin_ignored':  # lane 143: acts in main(): the thin capture is replayed without --thin
        return draws
    if plant == 'emu_shared_tables':  # lane 149: acts in census_frame(): fview runs with --thin-fault emu_shared_tables
        return draws
    fail(f'unknown --plant {plant}')


def row_key(d, level):
    """level 'class': class; 'tech': class + technique; 'site': class + technique + call site + pass site."""
    k = [d['class']]
    if level in ('tech', 'site'):
        k.append(d['technique'] or '-')
    if level == 'site':
        k.append('0x%08X' % d['call_site'] if d['call_site'] else '-')
        k.append('0x%08X' % d['pass_site'] if d['pass_site'] else '-')
    return tuple(k)


def owners(pixels, plant=''):
    """-> per sample (owner op or None, touched ops set). A node is a CONTENT draw when it is a draw, not a full-screen pass (those are followed by the walk
    to the surface they read) and it changed the pixel (or its value could not be read). The owner is the newest of them (the layer on top when the copies
    are followed down to the scene's own surface); touched = all of them (blended layers too)."""
    res = []
    for p in pixels:
        touched = set()
        for n in p['chain']:
            # S101 lane CJ: plant full_screen_by_count re-applies the OLD judge rule (<= 6 indices + full viewport = a pass, skipped): a far crowd card, a HUD sprite or a particle quad
            # then gives its pixels to the draw underneath ("cards own 0 %"). The shipped rule is judge.cpp's full_screen (a post origin or a read of the surface's own size).
            if n.get('kind') != 'draw' or n.get('full_screen') or (plant == 'full_screen_by_count' and n.get('quad_viewport')):
                continue
            if n.get('wrote') or not n.get('value_known', True):
                touched.add(n['op'])
        res.append((max(touched) if touched else None, touched))
    return res


def aggregate(draws, pixel_owners):
    """The tables. pixel_owners: list of (owner op, touched ops) per grid sample, or None when no pixels were taken."""
    by_op = {d['op']: d for d in draws}
    total = len(draws)
    out = {'draws': total, 'by_fate': dict(collections.Counter(d['fate'] for d in draws))}
    out['stream_draws'] = sum(1 for d in draws if d['fate'] == 'stream')
    out['promoted_draws'] = total - out['stream_draws']
    out['prims_total'] = sum(d['prims'] for d in draws)
    out['prims_stream'] = sum(d['prims'] for d in draws if d['fate'] == 'stream')
    samples = len(pixel_owners) if pixel_owners else 0
    own_by_op = collections.Counter()
    touch_by_op = collections.Counter()
    post_only = 0
    if pixel_owners:
        for owner, touched in pixel_owners:
            if owner is None:
                post_only += 1
            else:
                own_by_op[owner] += 1
            for t in touched:
                touch_by_op[t] += 1
    out['samples'] = samples
    out['post_copy_only'] = post_only
    out['samples_owned_by_stream'] = sum(n for op, n in own_by_op.items() if op in by_op and by_op[op]['fate'] == 'stream')
    out['samples_owned_by_ours'] = sum(n for op, n in own_by_op.items() if op in by_op and by_op[op]['fate'] != 'stream')
    out['samples_owned_unknown'] = sum(n for op, n in own_by_op.items() if op not in by_op)

    def table(level):
        touch_rows = collections.Counter()  # samples touched by at least one draw of the row
        if pixel_owners:
            for _, touched in pixel_owners:
                for k in {row_key(by_op[t], level) for t in touched if t in by_op}:
                    touch_rows[k] += 1
        rows = {}
        for d in draws:
            k = row_key(d, level)
            r = rows.setdefault(k, {'key': list(k), 'draws': 0, 'stream': 0, 'ours': 0, 'prims': 0, 'prims_stream': 0, 'own': 0, 'touch': 0, 'own_stream': 0})
            r['draws'] += 1
            r['prims'] += d['prims']
            if d['fate'] == 'stream':
                r['stream'] += 1
                r['prims_stream'] += d['prims']
            else:
                r['ours'] += 1
            r['own'] += own_by_op.get(d['op'], 0)
            if d['fate'] == 'stream':
                r['own_stream'] += own_by_op.get(d['op'], 0)
        for k, r in rows.items():
            r['touch'] = touch_rows.get(k, 0)
        return sorted(rows.values(), key=lambda r: (-r['own_stream'], -r['stream'], r['key']))

    out['by_class'] = table('class')
    out['by_technique'] = table('tech')
    out['by_site'] = table('site')
    ob = {}  # the stream's model parts by engine object (a wrestler, the ring, a prop: one object = one model)
    for d in draws:
        if d['fate'] != 'stream' or d['class'] != 'model_part' or not d['object']:
            continue
        r = ob.setdefault(d['object'], {'object': '0x%08X' % d['object'], 'draws': 0, 'parts': set(), 'prims': 0, 'own': 0, 'sites': set()})
        r['draws'] += 1
        r['parts'].add(d['part'])
        r['prims'] += d['prims']
        r['own'] += own_by_op.get(d['op'], 0)
        r['sites'].add('0x%08X/%s' % (d['pass_site'], d['technique'] or '-'))
    out['by_object'] = sorted(({**r, 'parts': len(r['parts']), 'sites': sorted(r['sites'])} for r in ob.values()), key=lambda r: (-r['own'], -r['prims']))
    tg = {}
    for d in draws:
        r = tg.setdefault(d['target'], {'target': d['target'], 'draws': 0, 'stream': 0, 'ours': 0, 'prims_stream': 0, 'viewport': d['viewport'], 'classes': collections.Counter()})
        r['draws'] += 1
        if d['fate'] == 'stream':
            r['stream'] += 1
            r['prims_stream'] += d['prims']
            r['classes'][d['class']] += 1
        else:
            r['ours'] += 1
    out['by_target'] = sorted(({**r, 'classes': dict(r['classes'])} for r in tg.values()), key=lambda r: -r['stream'])
    out['own_ops'] = {str(op): n for op, n in own_by_op.items()}
    return out


def pct(n, d):
    return '%5.1f%%' % (100.0 * n / d) if d else '    - '


def markdown(name, frame, agg, promote):
    s = agg['samples']
    L = [f'### {name}, frame {frame} (promotions on: {promote})', '']
    L.append(f"{agg['draws']} draws: **{agg['stream_draws']} still the stream's**, {agg['promoted_draws']} drawn by ours ({', '.join(f'{k} {v}' for k, v in sorted(agg['by_fate'].items()) if k != 'stream') or 'none'}). "
             f"Primitives (indices or vertices): {agg['prims_stream']} of {agg['prims_total']} on the stream.")
    if s:
        L.append(f"Picture ({s} grid samples): owned by the stream's draws {pct(agg['samples_owned_by_stream'], s).strip()}, by ours {pct(agg['samples_owned_by_ours'], s).strip()}, "
                 f"post/copy only {pct(agg['post_copy_only'], s).strip()}.")
    L.append('')

    def rows(title, key, cols, limit):
        L.append(f'**{title}**')
        L.append('')
        L.append('| ' + ' | '.join(cols) + ' | draws | stream | ours | stream prims | owns | owns (stream) | touches |')
        L.append('|' + '---|' * (len(cols) + 7))
        for r in agg[key][:limit]:
            L.append('| ' + ' | '.join(str(x) for x in r['key']) + f" | {r['draws']} | {r['stream']} | {r['ours']} | {r['prims_stream']} | {pct(r['own'], s)} | {pct(r['own_stream'], s)} | {pct(r['touch'], s)} |")
        L.append('')

    rows('By class', 'by_class', ['class'], 20)
    rows('By class and technique (the pass)', 'by_technique', ['class', 'technique'], 30)
    rows('By call site (the submitting function) and pass site', 'by_site', ['class', 'technique', 'call site', 'pass site'], 40)
    L.append('**Stream model parts by engine object (top 15 by pixels owned)**')
    L.append('')
    L.append('| object | draws | distinct parts | prims | owns | passes (pass site/technique) |')
    L.append('|---|---|---|---|---|---|')
    for r in agg['by_object'][:15]:
        L.append(f"| {r['object']} | {r['draws']} | {r['parts']} | {r['prims']} | {pct(r['own'], s)} | {', '.join(r['sites'])} |")
    L.append('')
    L.append('**By render target (stream draws)**')
    L.append('')
    L.append('| target (colour/depth image ids) | viewport | draws | stream | ours | stream prims | stream classes |')
    L.append('|---|---|---|---|---|---|---|')
    for r in agg['by_target'][:20]:
        L.append(f"| {r['target']} | {'x'.join(str(v) for v in r['viewport'][2:])} | {r['draws']} | {r['stream']} | {r['ours']} | {r['prims_stream']} | "
                 + ', '.join(f'{k} {v}' for k, v in sorted(r['classes'].items(), key=lambda kv: -kv[1])) + ' |')
    L.append('')
    return '\n'.join(L)


# ---------------------------------------------------------------- lane 143: thin captures and the proof bar
FRAME_HAS_THIN = 32   # adapter_frame.h FrameFlags::kFrameHasThin
THIN_CLASSES = ('arena', 'ring', 'wrestlers')
THIN_LINE = re.compile(r'\[fview\] thin frame (\d+): (\d+) lite draws, (\d+) not learned')


def frame_header_flags(frame_file: Path) -> int:
    """FrameFileHeader (adapter_frame.h): magic, version, capture_frame, section_count (u32 each), guest_frame (u64), present_interval, flags (u32) -> flags at byte 28."""
    import struct
    with open(frame_file, 'rb') as fh:
        b = fh.read(32)
    if len(b) < 32:
        fail(f'{frame_file}: shorter than a frame header')
    return struct.unpack('<IIIIQII', b)[6]


def thin_frames(cap: Path):
    """-> the capture-frame numbers (file order) whose header says kFrameHasThin. A capture with any of them is a thin capture."""
    fs = sorted((cap / 'frames').glob('*')) if (cap / 'frames').is_dir() else []
    return [i for i, f in enumerate(fs) if frame_header_flags(f) & FRAME_HAS_THIN]


def thin_arg(promote):
    """fview's --thin class list: the thin classes that are promoted (a class thin in the capture but not promoted is drawn by the stream's own draw, which has no geometry: the proof bar catches it)."""
    return [c for c in THIN_CLASSES if c in promote]


def proof_bar(redraw_png, live_png):
    """-> dict {ok, reasons, p999, ...}: the redraw of the last frame vs the live picture (proof_bar.py, owned_check's tolerance). A missing / unreadable picture is a failure, never a pass."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import proof_bar as PB
    for p in (redraw_png, live_png):
        if not Path(p).exists():
            return {'ok': False, 'reasons': [f'{p} is missing'], 'unreadable': True}
    ok, nums, reasons, d = PB.compare(redraw_png, live_png)
    return {'ok': ok, 'reasons': reasons, **nums}


# ---------------------------------------------------------------- one frame
def census_frame(fview, cap, frame, promote, grid, work, root, game, plant, timeout=1500, thin=()):
    flags = []
    for p in promote:
        flags.append(PROMOTIONS[p][0])
    if flags:
        flags += ['--game', game]
    if thin:   # lane 143: a thin capture: the lite draws are drawn from templates learned from the capture's own full frames
        flags += ['--thin', ','.join(thin)]
        if plant == 'emu_shared_tables':   # lane 149: the old fview emulation (see thin_draw.h Fault::EmuSharedTables); the third bind loses arena draws
            flags += ['--thin-fault', 'emu_shared_tables']
    base = [fview, '--capture', cap, '--frame', frame, '--root', root] + flags
    ops_json = work / f'ops_f{frame}.json'
    sec1, out1 = run(base + ['--ops', '--json', ops_json], timeout, f'fview ops frame {frame}')
    if not ops_json.exists():
        fail(f'fview wrote no ops json for frame {frame}')
    thin_lines = [m for m in (THIN_LINE.search(ln) for ln in out1.splitlines()) if m]   # lane 149: EVERY bind of this run (the picture below comes from a later bind: its not-learned count counts too)
    census = [ln for ln in out1.splitlines() if ' ours frames ' in ln or ln.startswith('[rdr][particles] frames') or 'replace classes' in ln or ln.startswith('[fview] crowd ours:') or ln.startswith('[rdr][crowd] cards:')]  # S101 lane 7; lane CD: the cards census line3: the census lines only
    owners_ = None
    if grid:
        gw, gh = grid
        jd = work / f'judge_f{frame}'
        pf = work / f'grid_f{frame}.txt'
        pf.write_text(f'grid {gw} {gh}\n')
        sec2, out2 = run(base + ['--judge-out', jd, '--judge-pixels', pf], timeout, f'fview judge frame {frame}')
        pj = jd / 'pixels.json'
        if not pj.exists():
            fail(f'fview wrote no pixels.json for frame {frame}')
        owners_ = owners(json.loads(pj.read_text(encoding='utf-8'))['pixels'], plant)
        thin_lines += [m for m in (THIN_LINE.search(ln) for ln in out2.splitlines()) if m]
    draws, op_count, kinds = load_draws(ops_json)
    if len(draws) != kinds.get('draw', len(draws)):
        fail(f'frame {frame}: {len(draws)} draw ops read, the frame says {kinds.get("draw")}')
    draws = mark_particle_heads(draws)
    draws = plant_draws(draws, plant)
    agg = aggregate(draws, owners_)
    agg['frame'] = int(frame)
    agg['ops'] = op_count
    agg['fview_census'] = census
    if thin:
        if not thin_lines:
            fail(f'frame {frame}: fview printed no "thin frame" line: this fview build is older than lane 143 (rebuild it with tools\fview\build.ps1) or the frame holds no thin draws')
        agg['thin'] = {'classes': list(thin), 'lite_draws': int(thin_lines[0].group(2)), 'not_learned': max(int(m.group(3)) for m in thin_lines), 'binds': len(thin_lines),
                       'not_learned_per_bind': [int(m.group(3)) for m in thin_lines]}   # lane 149: the worst bind decides
    return agg


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('input')
    ap.add_argument('--frames', default='last')
    ap.add_argument('--promote', default='auto', help='any of crowd,arena,wrestlers,ring,flash,props,screens,floor,cards,particles,ui (floor and cards need crowd) | none | auto (everything the capture allows: the classes with scene records, particles + ui without; cards are opt-in: auto,cards)')
    ap.add_argument('--grid', default='128x72', help='WxH grid of final-picture samples for the pixel share; 0 = none')
    ap.add_argument('--name')
    ap.add_argument('--out')
    ap.add_argument('--fview')
    ap.add_argument('--allow-stale', action='store_true', help='run although fview.exe is older than the renderer / fview sources (tools\\fview\\fview_exe.py)')
    ap.add_argument('--md')
    ap.add_argument('--plant', default='')
    ap.add_argument('--thin', default='auto', help='lane 143: auto = fview --thin for the promoted arena / ring / wrestlers when the capture holds thin draws (kFrameHasThin); off = never (the old behaviour)')
    ap.add_argument('--no-proof-bar', action='store_true', help="lane 143: skip the last-frame proof bar against the zip's picture.png (the output says UNPROVEN)")
    ap.add_argument('--timeout', type=int, default=1500)
    a = ap.parse_args()

    main_co = main_checkout()
    src = Path(a.input)
    name = a.name or (src.parent.name if src.suffix.lower() == '.zip' else src.name)
    out = Path(a.out) if a.out else main_co / 'out' / 's101' / 'leftovers' / name
    out.mkdir(parents=True, exist_ok=True)
    # S101 lane 84 (lane 79 extra 2): the default is the RELEASE build (the dev build goes stale); a worktree without one uses the main checkout's
    # review F6: this checkout's release, then this checkout's dev, then the main checkout's release (printed below)
    # lane 218: the shared locator (tools\fview\fview_exe.py): the newest own build (release or dev), else the main checkout's; REFUSES an exe older than the renderer sources.
    sys.path.insert(0, str(HERE / 'tools' / 'fview'))
    import fview_exe as fe
    try:
        fview = Path(fe.find_fview(prefer=a.fview, allow_stale=a.allow_stale, root=HERE, kind=None))
    except fe.FviewError as e:
        fail(str(e))
    print(f'[leftovers] fview: {fview}', flush=True)
    if not fview.exists():
        fail(f'{fview} not found: build it with tools\\fview\\build.ps1')
    cap = open_input(src, out)
    if not (cap / 'images.bin').exists():
        fail(f'{cap}: not a capture folder (no images.bin)')
    nframes = len(list((cap / 'frames').glob('*'))) if (cap / 'frames').is_dir() else 0
    frames = [str(max(nframes - 1, 0))] if a.frames == 'last' else a.frames.split(',')
    has_records = (cap / 'scene').is_dir() and any((cap / 'scene').glob('frame_*.rsc'))
    refused = []
    tokens = a.promote.split(',')
    if 'auto' in tokens:   # S101 lane CD: "auto,cards" = everything auto picks, plus the opt-in promotions named beside it
        promote = [p for p in ALL_PROMOTIONS if (has_records or not PROMOTIONS[p][1]) and p not in ('cards', 'caw')]   # lane 153 (review B1): caw is opt-in (auto,caw), like cards
        if 'floor' in promote and not ('crowd' in promote and records_hold_floor(cap)):   # floor: only with crowd, and only when the records hold floor members
            promote.remove('floor')
        refused = [p for p in ALL_PROMOTIONS if p not in promote and p not in ('floor', 'cards', 'caw')]
        for extra in tokens:
            if extra == 'auto':
                continue
            if extra not in ('cards', 'caw'):
                fail(f'--promote auto,{extra}: only cards and caw can be added to auto')
            if extra == 'caw':   # lane 153: the editor wrestler, opt-in (its count index would claim draws of a match capture)
                promote.append('caw')
                continue
            if 'crowd' not in promote:
                fail('--promote auto,cards needs crowd (the capture has no scene records)')
            promote.append('cards')
    elif a.promote == 'none':
        promote = []
    else:
        promote = a.promote.split(',')
        if 'floor' in promote and 'crowd' not in promote:
            fail('--promote floor needs crowd (fview --crowd-floor is only meaningful with --crowd-ours)')
        if 'cards' in promote and 'crowd' not in promote:
            fail('--promote cards needs crowd (fview --crowd-cards is only meaningful with --crowd-ours)')
        for p in promote:
            if p not in PROMOTIONS:
                fail(f'unknown promotion {p} (known: {", ".join(ALL_PROMOTIONS)})')
    need = [p for p in promote if PROMOTIONS[p][1]]
    if need and not has_records:
        fail(f"{cap}: --promote {','.join(need)} needs the capture's scene records (capture\\scene\\frame_*.rsc); this one has none")
    unavailable = []

    for cls, flag in (('ui', '--ui-ours'), ('ui_text', '--ui-text-ours')):   # lane 126 (review F7): ui_text is probed like ui (S101 lane 73's rule, now for both)
        if cls not in promote:
            continue   # the UI class is not in every build (replace_factories.h); auto drops it and says so, an explicit request fails
        pr = subprocess.run([str(fview), '--capture', str(cap), '--frame', '0', '--root', str(HERE), flag, '--game', str(main_co / 'game_data' / 'extracted'), '--ops',
                             '--json', str(out / f'probe_{cls}.json')], capture_output=True, text=True, timeout=600, errors='replace')
        if f'no {cls} class is registered' in pr.stdout + pr.stderr:
            if 'auto' not in tokens:
                fail(f'--promote {cls}: this fview build has no {cls} class registered (the contract is not merged into this branch)')
            promote.remove(cls)
            unavailable.append(cls)
    grid = None
    if a.grid != '0':
        gw, gh = a.grid.lower().split('x')
        grid = (int(gw), int(gh))
    game = str(main_co / 'game_data' / 'extracted')
    thin_set = thin_frames(cap)   # lane 143
    thin_cls = thin_arg(promote) if (thin_set and a.thin != 'off' and a.plant != 'thin_ignored') else []
    if thin_set:
        print(f'[leftovers] THIN capture: {len(thin_set)} of {nframes} frames hold lite draws (kFrameHasThin); '
              + (f'replaying with fview --thin {",".join(thin_cls)}' if thin_cls else 'fview --thin is OFF (--thin off / plant thin_ignored / no thin class promoted): the lite draws are dropped, the census is NOT a measurement'), flush=True)
    result = {'name': name, 'input': str(src), 'promote': promote, 'refused_no_records': refused, 'unavailable_in_build': unavailable, 'scene_records': has_records, 'grid': a.grid, 'plant': a.plant, 'frames': [], 'thin_capture': bool(thin_set), 'thin_frames': len(thin_set), 'thin_classes': thin_cls}
    mds = []
    for f in frames:
        agg = census_frame(str(fview), str(cap), f, promote, grid, out, str(HERE), game, a.plant, a.timeout, thin_cls)
        agg['complete'] = not (thin_set and (not thin_cls or agg.get('thin', {}).get('not_learned', 0) > 0))   # lane 143
        if not agg['complete']:
            why = (f"{agg['thin']['not_learned']} of {agg['thin']['lite_draws']} lite draws were not learned (drawn by nothing)" if thin_cls
                   else 'a thin capture replayed without --thin: its lite draws carry no geometry')
            print(f'[leftovers] FRAME {f} INCOMPLETE: {why}; its numbers are NOT a measurement', flush=True)
            agg['incomplete_why'] = why
        agg['promote'] = list(promote)  # S101 lane 73: the selftest's counter check reads which promotions this frame had on
        result['frames'].append(agg)
        label = ', '.join(promote) or 'none'
        if refused:
            label += ' (refused, the capture has no scene records: ' + ', '.join(refused) + ')'
        if unavailable:
            label += ' (not in this fview build: ' + ', '.join(unavailable) + ')'
        md = markdown(name, f, agg, label)
        mds.append(md)
        print(md)
    # lane 143: THE PROOF BAR: our redraw of the capture's last frame vs the zip's picture.png (the live game's own picture)
    live_png = next((c for c in (out / 'case' / 'picture.png', cap.parent / 'picture.png') if c.exists()), None)
    last = str(max(nframes - 1, 0))
    if a.no_proof_bar:
        result['proof_bar'] = {'ok': None, 'skipped': True}
        print('[leftovers] PROOF BAR SKIPPED (--no-proof-bar): nothing shows the redraw matches the live game: UNPROVEN', flush=True)
    elif not live_png:
        result['proof_bar'] = {'ok': None, 'skipped': True, 'why': 'no picture.png beside the capture (a bare capture folder)'}
        print('[leftovers] PROOF BAR not possible: no picture.png beside the capture (give the case zip): UNPROVEN', flush=True)
    else:
        if last in frames and grid:
            redraw_png = out / f'judge_f{last}' / 'picture.png'
        else:   # the last frame was not measured: a judge-only pass on it (4x4 grid) gives the picture
            gdir = out / 'proof_bar'
            gdir.mkdir(exist_ok=True)
            pf = gdir / 'grid.txt'
            pf.write_text('grid 4 4\n')
            fl = [PROMOTIONS[p][0] for p in promote] + (['--game', game] if promote else []) + (['--thin', ','.join(thin_cls)] if thin_cls else [])
            run([str(fview), '--capture', str(cap), '--frame', last, '--root', str(HERE)] + fl + ['--judge-out', gdir / 'jd', '--judge-pixels', pf], a.timeout, f'fview proof-bar picture frame {last}')
            redraw_png = gdir / 'jd' / 'picture.png'
        pb = proof_bar(redraw_png, live_png)
        pb['frame'] = int(last)
        pb['redraw'] = str(redraw_png)
        pb['live'] = str(live_png)
        result['proof_bar'] = pb
        if pb['ok']:
            print(f"[leftovers] PROOF BAR PASS: the redraw of frame {last} matches the live picture (guard block {pb['guard_block']:.1f}; strict owned_check tolerance {'met' if pb['strict_ok'] else 'not met'})", flush=True)
        else:
            print(f"[leftovers] PROOF BAR FAIL (UNPROVEN): the redraw of frame {last} does not match the live picture: {'; '.join(pb['reasons'])}", flush=True)
    bad = [x['frame'] for x in result['frames'] if not x['complete']]
    result['unproven'] = bool(bad) or result['proof_bar'].get('ok') is not True
    result['unproven_why'] = ([f'frames {bad} incomplete'] if bad else []) + ([] if result['proof_bar'].get('ok') is True else ['proof bar ' + ('failed' if result['proof_bar'].get('ok') is False else 'not run')])
    (out / 'leftovers.json').write_text(json.dumps(result, indent=1), encoding='utf-8')
    (out / 'leftovers.md').write_text('\n'.join(mds), encoding='utf-8')
    if a.md:
        Path(a.md).write_text('\n'.join(mds), encoding='utf-8')
    print(f'[leftovers] wrote {out / "leftovers.json"}')
    if result['unproven']:   # lane 143: loud and non-zero: a census that cannot show its redraw is the live picture is not a measurement
        print('[leftovers] FAIL: UNPROVEN: ' + '; '.join(result['unproven_why']))
        sys.exit(3 if (bad or result['proof_bar'].get('ok') is False) else 0)


if __name__ == '__main__':
    main()
