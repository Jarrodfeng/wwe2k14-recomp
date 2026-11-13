r"""S101 lane 196: selftest of tools\skip\caster_ledger.py (docs\renderer\SKIP_GAME_WORK_MAP.md sections 17 / 18). 0 game runs, Python only, about 40 s.

Runs the ledger on 50 frames (150:200) of Check B (run 7051) and run 7079 and asserts the numbers the sections quote, each plant against the check it must fail:
  casters  Check B: map site 93 draws a frame, rim 51, plane 32-40, circle 4; the rim pass is 100 % ready; the map pass is 62.4 % ready (the 34 map A draws a frame and a
           one-draw camera segment are not); the colour_off plant (the colour-pass source off) must drop the rim pass's ready share under 70 %;
           the offset0 plant (records read at offset 0) must drop it under 90 %.
  wrestlers  run 7079: palette rows of Brock (0xA90BC480) have no unsourced row; Check B: Brock's NONE share is over 15 % (the update-hook palette of run 7051: the
           past bug of WRESTLER_PROMOTION_MAP.md / SKIP_GAME_WORK_MAP.md 7.3, re-found); the three skeleton actors (0xA910B850, 0xA910E270, 0xA95B03F0) are over 50 %
           unsourced on both; the offset0 plant must take the record source to 0 (ready + held under 5 %).
SKIP (exit 0) when the captures are absent (local game data)."""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
TOOL = os.path.join(HERE, 'caster_ledger.py')


def main_checkout():
    # <main>\.claude\worktrees\<name>\tools\skip -> <main>; a main checkout is its own root
    parts = os.path.normpath(ROOT).split(os.sep)
    if '.claude' in parts:
        return os.sep.join(parts[:parts.index('.claude')])
    return ROOT


MAIN = main_checkout()
CHECKB = os.path.join(MAIN, 'out', 's101', 'lane92', 'case_20261111_215233_217_0_pc_run_exit')
R7079 = os.path.join(MAIN, 'out', 's101', 'lane92', 'case_20261112_122658_040_0_pc_run_exit')
OUT = os.path.join(MAIN, 'out', 'lane196_selftest')


def run(path, cls, name, plants=()):
    os.makedirs(OUT, exist_ok=True)
    js = os.path.join(OUT, name + '.json')
    cmd = [sys.executable, '-I', TOOL, path, '--class', cls, '--frames', '150:200', '-q', '--json', js]
    for p in plants:
        cmd += ['--plant', p]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    if r.returncode != 0:
        print('FAIL  tool exit %d: %s' % (r.returncode, (r.stdout + r.stderr).strip()[:300]))
        sys.exit(1)
    with open(js, encoding='utf-8') as fh:
        return json.load(fh)['sub']


def main():
    if not (os.path.isdir(os.path.join(CHECKB, 'capture', 'frames')) and os.path.isdir(os.path.join(R7079, 'capture', 'frames'))):
        print('SKIP  the Check B / run 7079 case folders are absent (local game data)')
        return 0
    bad = []

    def check(ok, msg):
        print(('PASS  ' if ok else 'FAIL  ') + msg)
        if not ok:
            bad.append(msg)

    c = run(CHECKB, 'casters', 'casters_b')
    pct = lambda s, k: 100.0 * s[k] / s['draws']
    check(c['map']['per_frame'] == 93.0 and c['rim']['per_frame'] == 51.0 and c['circle']['per_frame'] == 4.0 and 32 <= c['plane']['per_frame'] <= 40,
          'Check B draws a frame: map %.1f, rim %.1f, plane %.1f, circle %.1f (93 / 51 / 32-40 / 4)' % (c['map']['per_frame'], c['rim']['per_frame'], c['plane']['per_frame'], c['circle']['per_frame']))
    check(pct(c['rim'], 'ready') == 100.0, 'rim pass 100 %% ready (%.1f %%)' % pct(c['rim'], 'ready'))
    check(60.0 <= pct(c['map'], 'ready') <= 65.0, 'map pass about 62.4 %% ready (%.1f %%): map A (34 a frame) and the one-draw segment are not' % pct(c['map'], 'ready'))
    check(pct(c['plane'], 'ready') == 0.0, 'plane pass 0 %% ready (%.1f %%): its palette rows and EyePos have no source' % pct(c['plane'], 'ready'))
    p1 = run(CHECKB, 'casters', 'casters_b_colour_off', ('colour_off',))
    check(pct(p1['rim'], 'ready') < 70.0, 'plant colour_off: the rim pass falls to %.1f %% ready (< 70)' % pct(p1['rim'], 'ready'))
    p2 = run(CHECKB, 'casters', 'casters_b_offset0', ('offset0',))
    check(pct(p2['rim'], 'ready') < 90.0, 'plant offset0: the rim pass falls to %.1f %% ready (< 90)' % pct(p2['rim'], 'ready'))

    wb = run(CHECKB, 'wrestlers', 'wrestlers_b')['wrestler']
    wr = run(R7079, 'wrestlers', 'wrestlers_r')['wrestler']

    def none_share(S, obj):
        c_ = S['palette_obj'].get(obj, {})
        t = sum(c_.values())
        return 100.0 * c_.get('NONE', 0) / t if t else 0.0
    check(none_share(wr, 'A90BC480') == 0.0, 'run 7079: Brock palette NONE %.1f %% (0)' % none_share(wr, 'A90BC480'))
    check(none_share(wb, 'A90BC480') > 15.0, 'Check B: Brock palette NONE %.1f %% (> 15: the update-hook palette, the past bug re-found)' % none_share(wb, 'A90BC480'))
    for obj in ('A910B850', 'A910E270', 'A95B03F0'):
        check(none_share(wb, obj) > 50.0 and none_share(wr, obj) > 50.0, 'skeleton actor %s palette NONE %.1f %% (B) / %.1f %% (7079), both > 50' % (obj, none_share(wb, obj), none_share(wr, obj)))
    w0 = run(R7079, 'wrestlers', 'wrestlers_r_offset0', ('offset0',))['wrestler']
    check(100.0 * w0['ready_held'] / w0['draws'] < 5.0, 'plant offset0: wrestlers ready + held falls to %.1f %% (< 5)' % (100.0 * w0['ready_held'] / w0['draws']))
    print('PASS  caster_ledger selftest' if not bad else 'FAIL  %d check(s)' % len(bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
