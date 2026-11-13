r"""Selftest of tools\census\leftovers.py (S101 lane 58; gate lane `leftovers`).

Unit level (no GPU): the shipped functions (load_draws, plant_draws, owners, aggregate) on a hand-made ops json + pixels chain with known answers; planted twins
(a wrong fate, a doubled table) must change an answer the test pins.
Live level (needs Check B's case zip and a built fview.exe; a machine without them SKIPs, run directly it fails loudly): the shipped tool, through fview, on frames
199 and 200 of Check B (frame 200 has particle runs; 201 has none):
  - the draws counted equal the frame's draw ops, stream + ours equal the total, the same capture frame has the same draw count with promotions on and off;
  - INDEPENDENT count: fview's own census counters (cumulative per frame played: the arena's and wrestlers' `replaced`, the crowd's `stream draws dropped`) differ between
    the run to frame 199 and the run to frame 200 by exactly frame 200's draws with fate arena_ours / wrestlers_ours / crowd_ours;
  - the picture shares add up (stream + ours + post/copy only + unknown == samples) and the "none" arm owns nothing by ours;
  - planted twins through the real tool (--plant fate_swap, newfate_swap, dup_draws) each break one of those checks.
  py -I tools\census\leftovers_selftest.py [--zip PATH] [--unit-only]
"""
import argparse
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(HERE / 'tools' / 'census'))
import leftovers as L  # noqa: E402

problems = []
ALLP = 'crowd,arena,wrestlers,ring,flash,particles'  # S101 lane 73 (ui is not in every build; the tool's auto mode adds it when it is)


def check(cond, what):
    print(('ok   ' if cond else 'FAIL ') + what)
    if not cond:
        problems.append(what)


def draw(op, cls='model_part', fate='stream', tech='', call=0x82BDEF38, pas=0x82C0A950, obj=0xAB000000, part=0, prims=100, target='c1/d2', vs=None):
    return {'vs_variant': vs, 'op': op, 'kind': 'draw', 'fate': fate, 'class': cls, 'technique': tech, 'call_site': call, 'pass_site': pas, 'object': obj, 'part': part, 'widget': '',
            'prims': prims, 'viewport': [0, 0, 1280, 720], 'target': target}


def unit():
    ops = {'op_count': 9, 'kinds': {'draw': 5}, 'ops': [
        {'op': 0, 'kind': 'targets', 'colors': [7], 'depth': 8},
        {**draw(1, fate='arena_ours'), 'target': None},
        {**draw(2, cls='crowd', fate='crowd_ours', call=0x82518574, pas=0, obj=0), 'target': None},
        {**draw(3, cls='crowd', fate='stream', call=0x825130C4, pas=0, obj=0, tech='T_Audience2D', prims=4), 'target': None},
        {**draw(4, fate='stream', obj=0xAB000040), 'target': None},
        {**draw(5, cls='shadow', fate='stream', tech='T_ShadowMap', pas=0x82C04634), 'target': None},
    ]}
    with tempfile.TemporaryDirectory() as td:
        pj = Path(td) / 'ops.json'
        pj.write_text(json.dumps(ops))
        draws, total, kinds = L.load_draws(pj)
    check(len(draws) == 5 and kinds['draw'] == 5, 'unit: five draw ops read')
    check(all(d['target'] == 'c7/d8' for d in draws), 'unit: the target in force is attached to each draw')
    # four samples: 0 owned by op 4 (stream model part), 1 by op 1 (arena ours), 2 by op 2 (the crowd's first op, ours), 3 no content draw
    chains = [
        {'chain': [{'kind': 'draw', 'op': 4, 'wrote': True, 'full_screen': False}, {'kind': 'draw', 'op': 1, 'wrote': True, 'full_screen': False}]},  # op 4 newest
        {'chain': [{'kind': 'copy', 'op': 8}, {'kind': 'draw', 'op': 1, 'wrote': True, 'full_screen': False}]},
        {'chain': [{'kind': 'draw', 'op': 2, 'wrote': True, 'full_screen': False}, {'kind': 'draw', 'op': 7, 'wrote': True, 'full_screen': True}]},  # fs pass is no owner
        {'chain': [{'kind': 'draw', 'op': 7, 'wrote': True, 'full_screen': True}, {'kind': 'clear', 'op': 0}, {'kind': 'draw', 'op': 3, 'wrote': False, 'value_known': True, 'full_screen': False}]},
    ]
    # S101 lane CJ: a card frame. Op 3 is a stream card (4 indices, full viewport: quad_viewport) over op 1: the shipped rule credits it, the old index-count rule credits op 1.
    card = [{'chain': [{'kind': 'draw', 'op': 3, 'wrote': True, 'full_screen': False, 'quad_viewport': True}, {'kind': 'draw', 'op': 1, 'wrote': True, 'full_screen': False, 'quad_viewport': False}]},
            {'chain': [{'kind': 'draw', 'op': 7, 'wrote': True, 'full_screen': True, 'quad_viewport': True}, {'kind': 'draw', 'op': 1, 'wrote': True, 'full_screen': False, 'quad_viewport': False}]}]  # a real post pass stays skipped
    check([o for o, _ in L.owners(card)] == [3, 1], 'unit: a card (small quad, full viewport) owns its pixels; a full-screen pass still does not')
    check([o for o, _ in L.owners(card, 'full_screen_by_count')] == [1, 1], 'unit: planted full_screen_by_count (the old rule) credits the card pixels to the draw underneath')
    own = L.owners(chains)
    check([o for o, _ in own] == [4, 1, 2, None], 'unit: owners = the newest content draw (full-screen passes and non-writers are no owner)')
    a = L.aggregate(draws, own)
    check((a['draws'], a['stream_draws'], a['promoted_draws']) == (5, 3, 2), 'unit: 5 draws, 3 stream, 2 ours')
    check(a['by_fate'] == {'arena_ours': 1, 'crowd_ours': 1, 'stream': 3}, 'unit: by fate')
    check((a['samples_owned_by_stream'], a['samples_owned_by_ours'], a['post_copy_only']) == (1, 2, 1), 'unit: shares: 1 stream, 2 ours, 1 post/copy only')
    check(a['prims_stream'] == 204 and a['prims_total'] == 404, 'unit: primitives')
    cls = {tuple(r['key']): r for r in a['by_class']}
    check(cls[('model_part',)]['draws'] == 2 and cls[('model_part',)]['stream'] == 1 and cls[('model_part',)]['own'] == 2 and cls[('model_part',)]['own_stream'] == 1,
          'unit: model_part row (2 draws, 1 stream, owns 2 samples, 1 of them stream)')
    check(a['by_object'][0]['object'] == '0xAB000040' and a['by_object'][0]['own'] == 1, 'unit: the stream object table')
    # planted twins: the pinned answers must move
    for plant, field in (('fate_swap', 'stream_draws'), ('dup_draws', 'draws')):
        b = L.aggregate(L.plant_draws(draws, plant), own)
        check(b[field] != a[field], f'unit: planted {plant} moves {field} ({a[field]} -> {b[field]})')
    unit_new_fates()
    return


def unit_new_fates():
    """S101 lane 73: the fates added after lane 58 (ring_ours, flash_ours, ui_ours, particle_run) are ours; the first draw of a merged particle run (fate stream in fview, followed
    by folded draws) is relabelled particle_run_head and is ours; a run of one stays stream; the pixel share credits the run to its head."""
    ds = [draw(1, fate='ring_ours'), draw(2, fate='flash_ours', cls='sprite'), draw(3, fate='ui_ours', cls='sprite'),
          draw(4, cls='particle', fate='stream', prims=2), draw(5, cls='particle', fate='particle_run', prims=0), draw(6, cls='particle', fate='particle_run', prims=0),
          draw(7, cls='particle', fate='stream', prims=2, vs='yParticle_base_vertex'),  # a KEPT lone particle draw (the game's own vertex stage): not a head
          draw(8, cls='shadow', fate='stream'),
          draw(9, cls='particle', fate='stream', prims=2, vs='yParticleRuns_runs_vertex'),  # S101 lane 132: a MERGED lone draw (our run stage, nothing folded): a head
          draw(10, cls='particle', fate='stream', prims=2, vs='ySoftParticleRuns_runs_vertex')]  # ... and the soft one
    ds = L.mark_particle_heads(ds)
    fates = [d['fate'] for d in ds]
    check(fates == ['ring_ours', 'flash_ours', 'ui_ours', 'particle_run_head', 'particle_run', 'particle_run', 'stream', 'stream', 'particle_run_head', 'particle_run_head'], 'unit: the merged run head is relabelled (a merged lone draw too), a kept lone draw and a shadow stay stream')
    own = [(4, {4}), (1, {1}), (7, {7}), (8, {8})]  # the run's pixels sit on its head (op 4)
    a = L.aggregate(ds, own)
    check((a['draws'], a['stream_draws'], a['promoted_draws']) == (10, 2, 8), 'unit: ring / flash / ui / run heads + folded draws count as ours (8 of 10)')
    check((a['samples_owned_by_stream'], a['samples_owned_by_ours']) == (2, 2), 'unit: shares: the run head and the ring are ours, the lone particle and the shadow are the stream\'s')
    b = L.aggregate(L.plant_draws(ds, 'newfate_swap'), own)
    check(b['stream_draws'] == 10 and b['samples_owned_by_ours'] == 0, 'unit: planted newfate_swap reads every new fate as stream (10 stream, 0 samples ours)')
    check(a['by_fate'].get('particle_run_head') == 3 and a['by_fate'].get('particle_run') == 2, 'unit: by fate counts the head and the folded draws')
    # the independent check itself: a counters dict from census lines
    lines = ['[rdr][ring] ours frames 5, fallback 0, replaced 540, kept 1 (not ready 0)', '[rdr][flash] ours frames 2, fallback 0, replaced 12, kept 0 (x)',
             '[rdr][particles] frames 10, draws 400, runs 10 (merged draws 10, folding 410, particles 400), kept 0']
    c = counters(lines)
    check((c['ring'], c['flash'], c['particles'], c['particle_heads']) == (540, 12, 400, 10), 'unit: the counters parser reads the ring, flash and particle census lines')
    prev = {'fview_census': lines, 'promote': ['ring', 'flash', 'particles']}
    cur_lines = ['[rdr][ring] ours frames 6, fallback 0, replaced 648, kept 1 (not ready 0)', '[rdr][flash] ours frames 2, fallback 0, replaced 12, kept 0 (x)',
                 '[rdr][particles] frames 11, draws 484, runs 11 (merged draws 11, folding 494, particles 483), kept 0']
    cur = {'fview_census': cur_lines, 'promote': ['ring', 'flash', 'particles'],
           'by_fate': {'ring_ours': 108, 'particle_run': 83, 'particle_run_head': 1}}
    check(not deltas_problems(prev, cur), 'unit: the independent counter check passes on consistent counters and fate counts')
    bad = dict(cur, by_fate={'stream': 192})  # the new fates all read as stream
    check(len(deltas_problems(prev, bad)) == 3, 'unit: the counter check fails for ring, particle folds and run heads when the new fates are read as stream')


def counters(lines):
    """-> dict from the census lines of one fview run (cumulative up to and including the selected frame): the `replaced` counter of each replace hook (arena, wrestlers, ring,
    flash), the crowd's `stream draws dropped`, and the particle runs' folded draws (`folding` minus `merged draws`: a run's first draw is not folded). Missing line -> None."""
    out = {}
    for key in ('arena', 'wrestlers', 'ring', 'flash'):
        out[key] = None
        for ln in lines:
            m = re.match(r'\[rdr\]\[' + key + r'\] ours frames .*?replaced (\d+),', ln)
            if m:
                out[key] = int(m.group(1))
    out['crowd'] = None
    out['particles'] = out['particle_heads'] = None
    for ln in lines:
        m = re.match(r'\[rdr\]\[crowd\] ours frames .*?stream draws dropped (\d+),', ln)
        if m:
            out['crowd'] = int(m.group(1))
        m = re.match(r'\[rdr\]\[particles\] frames \d+, draws \d+, runs \d+ \(merged draws (\d+), folding (\d+),', ln)
        if m:
            out['particles'], out['particle_heads'] = int(m.group(2)) - int(m.group(1)), int(m.group(1))
    return out


def deltas_problems(prev, cur):
    """The independent check: the counters' delta from frame N-1 to N = the N+1 frame's draws by fate (arena, wrestlers, ring, flash replaced; crowd dropped; particle runs'
    folded draws and merged runs). A counter whose line is missing in a run (the hook never ran yet) counts as 0; only the promotions the run had on are checked."""
    p0, p1 = counters(prev['fview_census']), counters(cur['fview_census'])
    fates = {'arena': 'arena_ours', 'wrestlers': 'wrestlers_ours', 'crowd': 'crowd_ours', 'ring': 'ring_ours', 'flash': 'flash_ours', 'particles': 'particle_run'}   # ('floor' and 'props' add no counter of their own here: floor draws carry the crowd fates, props_ours is counted as promoted)
    out = []
    for name, fate in fates.items():
        if name not in cur['promote']:
            continue
        want = (p1[name] or 0) - (p0[name] or 0)
        got = cur['by_fate'].get(fate, 0) + (cur['by_fate'].get('crowd_injected', 0) if name == 'crowd' else 0)  # the injecting op is a dropped crowd draw too
        if want != got:
            out.append(f'{name}: fview counters grew by {want}, the tool counted {got} draws of fate {fate}')
    if 'particles' in cur['promote']:
        want, got = (p1['particle_heads'] or 0) - (p0['particle_heads'] or 0), cur['by_fate'].get('particle_run_head', 0)
        if want != got:
            out.append(f'particle run heads: fview merged runs grew by {want}, the tool counted {got}')
    return out


def tool(zip_path, work, frames, promote, grid, plant='', name='selftest'):
    cmd = [sys.executable, '-I', str(HERE / 'tools' / 'census' / 'leftovers.py'), str(zip_path), '--frames', frames, '--promote', promote, '--grid', grid, '--out', str(work), '--name', name, '--fview', str(HERE / 'out' / 'renderer' / 'dev' / 'fview.exe'), '--no-proof-bar']   # lane 143: the proof bar has its own selftest (thin_census_selftest.py); S101 lane 84: the tool now defaults to the release build; the selftest checks the tree's own dev build
    if plant:
        cmd += ['--plant', plant]
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=3000)
    if p.returncode != 0:
        print(p.stdout[-800:], p.stderr[-800:])
        problems.append(f'the tool failed ({" ".join(cmd[-8:])})')
        return None
    return json.loads((Path(work) / 'leftovers.json').read_text(encoding='utf-8'))


def live(zip_path):
    base = Path(tempfile.mkdtemp(prefix='leftovers_selftest_', dir=str(HERE / 'out'))) if (HERE / 'out').is_dir() else Path(tempfile.mkdtemp())
    aprev = tool(zip_path, base / 'aprev', '199', ALLP, '0')
    zip_path = base / 'aprev' / 'case'  # the first call extracted the zip here: the other calls read that folder (600 MB are not unpacked five times)
    acur = tool(zip_path, base / 'acur', '200', ALLP, '16x9')
    if not aprev or not acur:
        return
    fprev, fcur = aprev['frames'][0], acur['frames'][0]
    check(fcur['draws'] > 0 and fcur['promoted_draws'] > 0, f"live: frame 200 has {fcur['draws']} draws, {fcur['promoted_draws']} drawn by ours")
    check(fcur['stream_draws'] + fcur['promoted_draws'] == fcur['draws'], 'live: stream + ours == draws')
    check(not deltas_problems(fprev, fcur), 'live: fview\'s own counters (frame 199 -> 200) equal the tool\'s fate counts: ' + '; '.join(deltas_problems(fprev, fcur)) if deltas_problems(fprev, fcur)
          else 'live: fview\'s own counters (frame 199 -> 200) equal the tool\'s fate counts (arena, wrestlers, crowd, ring, flash, particles)')
    s = fcur['samples']
    check(s == 144 and fcur['samples_owned_by_stream'] + fcur['samples_owned_by_ours'] + fcur['post_copy_only'] + fcur['samples_owned_unknown'] == s,
          f"live: picture shares add up ({fcur['samples_owned_by_stream']} stream + {fcur['samples_owned_by_ours']} ours + {fcur['post_copy_only']} post/copy + {fcur['samples_owned_unknown']} unknown = {s})")
    check(fcur['samples_owned_unknown'] == 0, 'live: every owner op is a draw of the table')
    ncur = tool(zip_path, base / 'ncur', '200', 'none', '16x9')
    if ncur:
        g = ncur['frames'][0]
        check(g['draws'] == fcur['draws'], f"live: the same frame has the same draw count with promotions off ({g['draws']} vs {fcur['draws']})")
        check(g['promoted_draws'] == 0 and g['samples_owned_by_ours'] == 0, 'live: promotions off: nothing is ours')
    # planted twins through the real tool
    pf = tool(zip_path, base / 'pf', '200', ALLP, '0', plant='fate_swap')
    if pf:
        pr = deltas_problems(fprev, pf['frames'][0])
        check(bool(pr), 'planted fate_swap: the independent counter check fails (' + (pr[0] if pr else 'it did NOT fail') + ')')
    check(fcur['by_fate'].get('ring_ours', 0) > 0 and fcur['by_fate'].get('particle_run', 0) > 0,
          f"live: frame 200 has ring_ours {fcur['by_fate'].get('ring_ours', 0)} and particle_run {fcur['by_fate'].get('particle_run', 0)} draws (the new fates are not vacuous)")
    pn = tool(zip_path, base / 'pn', '200', ALLP, '0', plant='newfate_swap')  # S101 lane 73: only the NEW fates read as stream: the new counters must catch it
    if pn:
        pr = deltas_problems(fprev, pn['frames'][0])
        names = {x.split(':')[0] for x in pr}
        check({'ring', 'particles'} <= names, 'planted newfate_swap: the independent counter check fails on ring and the particle folds (' + ('; '.join(pr) if pr else 'it did NOT fail') + ')')
        check(not ({'arena', 'wrestlers', 'crowd'} & names), 'planted newfate_swap: the old fates are still counted right (arena / wrestlers / crowd checks stay quiet)')
    pd = tool(zip_path, base / 'pd', '200', ALLP, '0', plant='dup_draws')
    if pd:
        d = pd['frames'][0]
        check(d['draws'] != fcur['draws'], f"planted dup_draws: the draw count moves off the frame's ({d['draws']} vs {fcur['draws']})")
        check(bool(deltas_problems(fprev, d)), 'planted dup_draws: the independent counter check fails too')


def floor_unit():
    """S101 lane 84: `floor` is a promotion (fview --crowd-floor, counted under the crowd's fates) that needs crowd; auto includes it only when the records hold floor members."""
    import tempfile
    check('floor' in L.PROMOTIONS and L.PROMOTIONS['floor'][0] == '--crowd-floor' and L.PROMOTIONS['floor'][1], 'unit: floor is a promotion (--crowd-floor, needs scene records)')
    check('crowd_ours' in L.PROMOTED and 'crowd_injected' in L.PROMOTED, 'unit: the floor draws carry crowd_ours / crowd_injected, which count as ours')
    with tempfile.TemporaryDirectory() as t:
        cap = Path(t) / 'capture'
        (cap / 'scene').mkdir(parents=True)
        for i in range(8):
            (cap / 'scene' / f'frame_{i:06d}.rsc').write_bytes(b'RSC0 crowd:body:1:member:3 ')
        check(not L.records_hold_floor(cap), 'unit: records without floor members -> auto leaves floor out')
        (cap / 'scene' / 'frame_000007.rsc').write_bytes(b'RSC0 crowd:floor:2:member:5 ')
        check(L.records_hold_floor(cap), 'unit: records with floor members -> auto includes floor')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--zip')
    ap.add_argument('--unit-only', action='store_true')
    a = ap.parse_args()
    unit()
    floor_unit()
    if not a.unit_only:
        main_co = L.main_checkout()
        z = Path(a.zip) if a.zip else next(iter((main_co / 'out' / 's101' / 'runs' / '20261111_215201_s100_owner_s89_brock').glob('case_*.zip')), None)
        sys.path.insert(0, str(HERE / 'tools' / 'fview'))   # lane 218: the shared locator (refuses an exe older than the renderer sources)
        import fview_exe as fe
        try:
            fview = Path(fe.find_fview(root=HERE, kind='dev'))
        except fe.FviewError as _e:
            fview = HERE / 'out' / 'renderer' / 'dev' / 'fview.exe'
            problems.append(str(_e))
        if not z or not z.exists() or not fview.exists():
            print(f'SKIP live level: Check B zip {z} or {fview} missing')
            if a.zip:
                problems.append('the named zip or fview.exe is missing')
        else:
            live(z)
    if problems:
        print(f'FAIL: {len(problems)} problem(s)')
        for p in problems:
            print('  -', p)
        sys.exit(1)
    print('leftovers selftest: all checks passed')


if __name__ == '__main__':
    main()
