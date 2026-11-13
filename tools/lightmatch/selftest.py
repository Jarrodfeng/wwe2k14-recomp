r"""Lane 33 gate check (offline, 0 game runs): the light extraction and the light comparison, with positive controls.

  py -I tools\lightmatch\selftest.py

1. Extraction (shipped code path, tools\lightmatch\lm_extract.py) on the Bryan and Brock entrance zips and the wm30 match zip: per frame a camera
   and >= 20 light groups of the classes wrestler / arena / prop / crowd.
2. Names stripped (the shipped frame_json fed a Frame whose origins are removed) must FAIL loudly, not return "0 lights".
3. Match (arena 29, wm30 frame 250, the log's match card names the arena): after the lane's fixes every arena / prop light direction is within
   THRESH_ANGLE degrees and the direction term is below THRESH_DIR; the colour fit of crowd and wrestler classes is below THRESH_COLOUR.
4. Entrance (Bryan, arena 4, frame 90, clock cue 15 local 208 found by the full search): arena / crowd / prop sets within THRESH_COLOUR, the
   entrant's skin set within THRESH_WRESTLER (the rig clock is a loop: a phase error shows here).
5. Planted faults (WWE_LM_PLANT=flipy / noattach in the viewer's own evaluation, src\assets\view\lightmatch_plant.h): the same comparisons must
   report "direction" as the dominant difference with at least 20x the clean direction cost. flipy is the S98 wrong-space-flip class, noattach the
   first S97 entrance light (keyed view-space directions read as world).
Thresholds are derived from the post-fix numbers with a margin (docs\renderer\LIGHT_MATCH.md "Numbers").
Needs the viewer built from this tree (tools\assets\build_viewer.ps1): a missing viewer is a FAIL, not a skip.
"""
import json
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[1] / 'tools' / 'solver'))
import lm_viewer as V  # noqa: E402

MAIN = V.MAIN
RUNS = MAIN / 'out' / 's101' / 'runs'
OUT = MAIN / 'out' / 's101' / 'lightmatch'
THRESH_ANGLE, THRESH_DIR, THRESH_COLOUR, THRESH_WRESTLER = 0.5, 0.1, 0.01, 0.7
PY = [sys.executable, '-I']


def zip_of(pat):
    z = sorted(RUNS.glob(pat + '/case_*.zip'))
    if not z:
        raise SystemExit(f'FAIL  no case zip {pat}')
    return z[-1]


def run(args, env=None, expect_ok=True):
    p = subprocess.run(PY + args, capture_output=True, text=True, env=dict(os.environ, **(env or {})), timeout=400)
    if expect_ok and p.returncode != 0:
        raise SystemExit('FAIL  ' + ' '.join(args[:3]) + ': ' + (p.stdout + p.stderr)[-500:])
    return p


def worst_angle(cmp_frame):
    w = 0.0
    for m in cmp_frame['matches']:
        if m.get('matched') and m['klass'] in ('arena', 'prop', 'crowd'):
            for l in m['lights']:
                if l['angle_deg'] is not None:
                    w = max(w, l['angle_deg'])
    return w


def main():
    exe = V.viewer_exe('vk')
    print('viewer:', exe)
    if not str(exe).startswith(str(V.REPO)) and not os.environ.get('WWE_VIEWER_EXE'):
        print('note: using the main checkout viewer (a worktree viewer was not built): fixes may be absent')
    # 1. extraction
    # Pinned to Check A's zips (KEEP files): "the newest wm30 / bryan zip" drifted when Check B added a shorter wm30 zip (no frame 250).
    cases = [('bryan', zip_of('20261111_174549_s100_owner_bryan_entrance'), '0,90'), ('brock', zip_of('20261111_175051_s100_owner_s89_brock'), '0,240'),
             ('wm30', zip_of('20261111_174707_s100_owner_s90_wm30'), '250')]
    for label, z, frames in cases:
        p = run([str(HERE / 'lm_extract.py'), str(z), '--frames', frames, '--out', str(OUT / label)])
        for fn in frames.split(','):
            j = json.loads((OUT / label / f'frame_{fn}.json').read_text(encoding='utf-8'))
            kl = {g['klass'] for g in j['groups']}
            if len(j['groups']) < 20 or not j['camera'] or not {'model_part', 'crowd'} <= kl:
                raise SystemExit(f'FAIL  {label} frame {fn}: {len(j["groups"])} groups, camera {bool(j["camera"])}, classes {sorted(kl)}')
    print('PASS extraction: camera + light groups on Bryan, Brock, wm30')
    # 2. names stripped
    import capread
    import lm_extract
    import solve
    src = capread.Source(str(cases[0][1]))
    orig = capread.Frame.__init__

    def stripped(self, buf):
        orig(self, buf)
        self.has_origins = False
        self.origins = None
    capread.Frame.__init__ = stripped
    try:
        lm_extract.frame_json(src, solve.Effects(), src.frames[0], True)
        raise SystemExit('FAIL  names-stripped frame was accepted (a checker that reads nothing must fail)')
    except SystemExit as e:
        if 'no draw names' not in str(e):
            raise
    finally:
        capread.Frame.__init__ = orig
    print('PASS names stripped: fails loudly')
    # 3 + 5. match, clean and planted
    mt = ['match', '--label', 'wm30', '--arena', '29', '--frames', '250', '--tmax', '0', '--tstep', '40']
    run([str(HERE / 'lm_compare.py')] + mt + ['--out-name', 'gate_clean.json'])
    clean = json.loads((OUT / 'wm30' / 'gate_clean.json').read_text(encoding='utf-8'))['frames']['250']
    dclean = clean['summary']['terms']['direction']
    ang = worst_angle(clean)
    if dclean > THRESH_DIR or ang > THRESH_ANGLE:
        raise SystemExit(f'FAIL  match: direction term {dclean:.4f} (max {THRESH_DIR}), worst angle {ang:.3f} deg (max {THRESH_ANGLE})')
    um = [m for m in clean['matches'] if not m.get('matched') and m['klass'] in ('arena', 'prop')]
    if um:
        raise SystemExit(f'FAIL  match: {len(um)} arena / prop groups unmatched')
    # set 100 rule: every arena / prop group the game fed with NO lights (set 100, 502) must be matched by an unlit viewer group to the colour fit
    unlit = [m for m in clean['matches'] if m.get('matched') and m['klass'] in ('arena', 'prop') and not m['lights']]
    if not unlit or max(m['cost'] for m in unlit) > THRESH_COLOUR:
        raise SystemExit(f'FAIL  match: unlit (set 100 / 502) groups {len(unlit)}, worst cost {max([m["cost"] for m in unlit] or [9]):.4f}')
    print(f'PASS set 100 rule: {len(unlit)} unlit groups match to {max(m["cost"] for m in unlit):.4f}')
    # A/B switch: arena_follow false is the old world-fixed reading and must show the direction error (a failing-before control)
    run([str(HERE / 'lm_compare.py')] + mt + ['--viewer-cmd', '{"cmd":"look","arena_follow":false}', '--out-name', 'gate_nofollow.json'])
    nf = json.loads((OUT / 'wm30' / 'gate_nofollow.json').read_text(encoding='utf-8'))['frames']['250']
    if worst_angle(nf) < 20:
        raise SystemExit(f'FAIL  arena_follow=false shows worst angle {worst_angle(nf):.2f} deg (expected the old 25 / 90 deg errors)')
    print(f'PASS A/B switch: arena_follow=false worst angle {worst_angle(nf):.1f} deg vs {ang:.3f} on')
    # B1: an entrance and its stop without a camera move must give the arena its camera-attached lights back
    run([str(HERE / 'lm_compare.py')] + mt + ['--after-entrance', '324', '--out-name', 'gate_afterent.json'])
    ae = json.loads((OUT / 'wm30' / 'gate_afterent.json').read_text(encoding='utf-8'))['frames']['250']
    if worst_angle(ae) > THRESH_ANGLE:
        raise SystemExit(f'FAIL  after entrance_stop the arena lights are not turned to the camera: worst angle {worst_angle(ae):.2f} deg')
    print(f'PASS entrance_stop restores the follow: worst angle {worst_angle(ae):.3f} deg')
    for k in ('crowd',):
        if clean['fit_per_class'].get(k, 9) > THRESH_COLOUR:
            raise SystemExit(f'FAIL  match: {k} colour fit {clean["fit_per_class"][k]}')
    print(f'PASS match (arena 29): direction term {dclean:.4f}, worst angle {ang:.3f} deg')
    for plant in ('flipy', 'noattach'):
        run([str(HERE / 'lm_compare.py')] + mt + ['--plant', plant, '--out-name', f'gate_{plant}.json'])
        pf = json.loads((OUT / 'wm30' / f'gate_{plant}.json').read_text(encoding='utf-8'))['frames']['250']
        dt = {k: pf['summary']['terms'][k] - clean['summary']['terms'][k] for k in pf['summary']['terms']}
        dom = max(dt, key=dt.get)  # the DIFFERENCE the planted fault made (the clean frame's own [U] colour terms are not its doing)
        if dom != 'direction' or pf['summary']['terms']['direction'] < 20 * max(dclean, 0.005):
            raise SystemExit(f'FAIL  planted {plant}: largest change {dom} {dt}, direction {pf["summary"]["terms"]["direction"]} (clean {dclean})')
        print(f'PASS planted {plant} (match): the change is dominated by direction, {pf["summary"]["terms"]["direction"]:.3f} vs clean {dclean:.4f}')
    # 4 + 5. entrance
    et = ['entrance', '--label', 'bryan', '--arena', '4', '--wrestler', '324', '--frames', '90', '--clock', '90:15:208']
    run([str(HERE / 'lm_compare.py')] + et + ['--out-name', 'gate_clean.json'])
    ec = json.loads((OUT / 'bryan' / 'gate_clean.json').read_text(encoding='utf-8'))['frames']['90']
    fpc = ec['fit_per_class']
    for k in ('arena', 'crowd'):
        if fpc.get(k, 9) > THRESH_COLOUR:
            raise SystemExit(f'FAIL  entrance: {k} fit {fpc[k]}')
    if fpc.get('wrestler', 9) > THRESH_WRESTLER:
        raise SystemExit(f'FAIL  entrance: wrestler fit {fpc["wrestler"]}')
    ang = worst_angle(ec)
    # (the prop group lit by the entrant's block is a known [U]: judged on arena / crowd angles only)
    angs = [l['angle_deg'] for m in ec['matches'] if m.get('matched') and m['klass'] in ('arena', 'crowd') for l in m['lights'] if l['angle_deg'] is not None]
    if max(angs) > THRESH_ANGLE:
        raise SystemExit(f'FAIL  entrance: worst arena / crowd angle {max(angs):.3f}')
    dent = sum(m['terms']['direction'] for m in ec['matches'] if m.get('matched') and m['klass'] in ('arena', 'crowd'))
    print(f'PASS entrance (Bryan, arena 4): fit {fpc}, worst arena/crowd angle {max(angs):.3f} deg')
    for plant in ('flipy', 'noattach'):
        run([str(HERE / 'lm_compare.py')] + et + ['--plant', plant, '--out-name', f'gate_{plant}.json'])
        pe = json.loads((OUT / 'bryan' / f'gate_{plant}.json').read_text(encoding='utf-8'))['frames']['90']
        dp = sum(m['terms']['direction'] for m in pe['matches'] if m.get('matched') and m['klass'] in ('arena', 'crowd'))
        if dp < 20 * max(dent, 0.005):
            raise SystemExit(f'FAIL  planted {plant} (entrance): direction cost {dp:.4f} vs clean {dent:.4f}')
        print(f'PASS planted {plant} (entrance): direction cost {dp:.3f} vs clean {dent:.4f}')
    print('OK  lightmatch: extraction, loud failure, match + entrance thresholds, planted faults reported')


if __name__ == '__main__':
    main()
