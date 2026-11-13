r"""The judge's acceptance test (Session 101 lane 20; gate checks judge.build / judge.selftest). Needs out\renderer\dev\fview.exe (tools\fview\build.ps1) and the fixture zip in
the MAIN checkout (out\s101\judge\fixture\brock_exit_f0-200.zip, KEEP: a Check A zip, capture frames 0..200). 0 game runs: fview only. A check that cannot read its input FAILS.

 1 same arm twice      fview vs fview, frame 5: 0 regions and 0 differing pixels (two fview arms are deterministic; threshold 1)
 2 the S100 far crowd  the 29 effects' "alpha_test": false planted again (SESSION100_SUMMARY.md 14:23 entry; RENDERER_ARC.md 2.1): frame 5: every region is crowd, cause yAudience;
                       frame 200 (1924 particles, also planted): the biggest region's cause is yAudience, not yParticle (earliest op where the arms differ)
 3 another class       yMovieDefault drawing half brightness (a .frag edit): the biggest region and >= 90% of the changed area is video
 4 unreached plant     half brightness in yLiquidMap, an effect frame 5 never draws: 0 regions (both arms take the same path), 0 differing pixels
 5 plant applies none  an edit that changes no SPIR-V (a comment) must FAIL the run, naming "the plant applies nothing"
 6 class map           every effects\*.fx.json effect maps to a class other than "other" / "unknown"; yDefault is model, yCombineTex post, yFlashDefault crowd
 7 time                seconds per frame are printed (and stay under 300 s)
Tests 2-5 are the planted-fault twins of test 1 (LESSONS: a "0 problems" check needs a twin that fails).
"""
import json
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parents[2]
MAIN = Path(os.environ.get('RDR_MAIN_CHECKOUT', HERE))
ZIP = MAIN / 'out' / 's101' / 'judge' / 'fixture' / 'brock_exit_f0-200.zip'
results = []


def report(name, ok, evidence):
    results.append(ok)
    print(f'{"PASS" if ok else "FAIL"}  {name}: {evidence}', flush=True)


def judge(frames, args, out):
    cmd = ['py', '-I', str(HERE / 'tools' / 'judge' / 'judge.py'), str(ZIP), '--frames', frames, '--out', str(out)] + args
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=2000, cwd=HERE)
    text = p.stdout + p.stderr
    if p.returncode != 0:
        return None, text[-700:]
    return json.loads((out / 'judge.json').read_text(encoding='utf-8')), text


def main():
    sys.path.insert(0, str(HERE / 'tools' / 'fview'))   # lane 218: the shared locator (refuses an exe older than the renderer sources)
    import fview_exe as fe
    exe = Path(fe.find_or_exit(root=HERE, kind='dev'))
    if not exe.exists():
        print(f'FAIL  fview.exe missing: {exe} (run tools\\fview\\build.ps1)')
        return 1
    if not ZIP.exists():
        print(f'FAIL  the fixture zip is missing: {ZIP}')
        return 1
    work = MAIN / 'out' / 's101' / 'judge' / '_selftest'
    work.mkdir(parents=True, exist_ok=True)
    sys.path.insert(0, str(HERE / 'tools' / 'judge'))
    import judge as judge_mod
    import plant

    # 6 class map (offline)
    fx = sorted(p.name[:-len('.fx.json')] for p in (HERE / 'src' / 'renderer' / 'shaders' / 'fx').glob('*.fx.json'))
    bad = [e for e in fx if judge_mod.effect_class(e) in ('other', 'unknown')]
    pinned = (judge_mod.effect_class('yDefault'), judge_mod.effect_class('yCombineTex'), judge_mod.effect_class('yFlashDefault'))
    report('class map: every effect has a class; yDefault / yCombineTex / yFlashDefault pinned', not bad and len(fx) > 50 and pinned == ('model', 'post', 'crowd'),
           f'{len(fx)} effects, unmapped {bad}, pinned {pinned}')

    j1, text = judge('5', ['--arm-a', 'fview', '--arm-b', 'fview'], work / 't1')
    f1 = j1['frames'][0] if j1 else {}
    report('same arm twice: 0 regions, 0 differing pixels', j1 is not None and f1['regions_total'] == 0 and f1['changed_pixels'] == 0,
           f'{f1.get("regions_total")} regions, {f1.get("changed_pixels")} px differ (threshold {j1["threshold"] if j1 else "?"}), {f1.get("seconds")} s' if j1 else f'judge failed: {text}')

    plant.alpha_false(work / 'fx_alpha_false')
    j2, text = judge('5,200', ['--arm-a', 'fview', '--arm-b', f'fview:fx={work / "fx_alpha_false"}'], work / 't2')
    if j2:
        r5, r200 = j2['frames'][0]['regions'], j2['frames'][1]['regions']
        # (with threshold 1 the planted shadow / depth effects also move a few arena and bloom pixels: the crowd is the biggest region and nearly all of the changed area)
        share = lambda regs: sum(r['pixels'] for r in regs if r['class'] == 'crowd') / max(1, sum(r['pixels'] for r in regs))
        ok5 = bool(r5) and r5[0]['class'] == 'crowd' and r5[0].get('cause_effect') == 'yAudience' and bool(r5[0].get('origin')) and share(r5) >= 0.9
        ok200 = bool(r200) and r200[0]['class'] == 'crowd' and r200[0].get('cause_effect') == 'yAudience' and share(r200) >= 0.9
        ev = (f'frame 5: {len(r5)} regions, crowd share {share(r5) if r5 else 0:.2f}, biggest cause {r5[0].get("cause_effect") if r5 else None}; frame 200: {len(r200)} regions, crowd share {share(r200) if r200 else 0:.2f}, biggest cause '
              f'{r200[0].get("cause_effect") if r200 else None} (visible {r200[0].get("visible_class") if r200 else None}), {j2["frames"][0]["seconds"]} s / {j2["frames"][1]["seconds"]} s')
        ok = ok5 and ok200
    else:
        ok, ev = False, f'judge failed: {text}'
    report('S100 far crowd (alpha_test false in 29 effects): crowd / yAudience on frames 5 and 200', ok, ev)

    plant.color_scale(work / 'fx_movie_half', 'yMovieDefault')
    j3, text = judge('5', ['--arm-a', 'fview', '--arm-b', f'fview:fx={work / "fx_movie_half"}'], work / 't3')
    if j3:
        regs = j3['frames'][0]['regions']
        classes = sorted({r['class'] for r in regs})
        vid = sum(r['pixels'] for r in regs if r['class'] == 'video') / max(1, sum(r['pixels'] for r in regs))
        ok = bool(regs) and regs[0]['class'] == 'video' and regs[0].get('cause_effect') == 'yMovieDefault' and vid >= 0.9
        ev = f'{len(regs)} regions, classes {classes}, video share {vid:.2f}, {j3["frames"][0]["seconds"]} s'
    else:
        ok, ev = False, f'judge failed: {text}'
    report('second fault (yMovieDefault half brightness) is named video, nothing else', ok, ev)

    plant.color_scale(work / 'fx_liquid_half', 'yLiquidMap')
    j4, text = judge('5', ['--arm-a', 'fview', '--arm-b', f'fview:fx={work / "fx_liquid_half"}'], work / 't4')
    f4 = j4['frames'][0] if j4 else {}
    report('a plant in an effect the frame never draws: 0 regions, 0 differing pixels', j4 is not None and f4['regions_total'] == 0 and f4['changed_pixels'] == 0,
           f'{f4.get("regions_total")} regions, {f4.get("changed_pixels")} px differ, {f4.get("seconds")} s' if j4 else f'judge failed: {text}')

    plant.noop(work / 'fx_noop', 'yLiquidMap')
    j5, text = judge('5', ['--arm-a', 'fview', '--arm-b', f'fview:fx={work / "fx_noop"}'], work / 't5')
    report('a plant that changes no shader fails the run', j5 is None and 'the plant applies nothing' in text, text.strip().splitlines()[-1][:160] if text.strip() else 'no output')

    secs = [f['seconds'] for j in (j1, j2, j3, j4) if j for f in j['frames']]
    report('time per frame stated, under 300 s', bool(secs) and max(secs) < 300, 'seconds per frame: ' + ', '.join(str(s) for s in secs))
    bad = results.count(False)
    print(f'judge selftest: {len(results) - bad} of {len(results)} pass')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
