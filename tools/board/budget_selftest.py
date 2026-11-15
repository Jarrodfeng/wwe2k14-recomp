#!/usr/bin/env python3
"""Selftest of the budget board and the one-line bench (S102 lane M3). Exit 0 only when every check holds. No game, no Surface.
  py -I tools\\board\\budget.py --selftest
Fixtures: tools\\board\\fixtures\\bench (the shape of out\\s101\\surface_rebench\\y: trimmed real frames CSVs / log lines / power.csv, numbers only) and
small made-up ledger / lab / trains / lane files built in a temp folder. Every "refused" check is a planted twin: the same input with one thing removed
(the past bug: on release day 2026-11-13 the Surface bench had no thermal state and no settings named, so its numbers were not numbers).
"""
import csv, json, shutil, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / 'surface'))
import budget as B
import bench_line as BL

FIX = HERE / 'fixtures' / 'bench'
FAILS = []


def ok(cond, text):
    print(('ok    ' if cond else 'FAIL  ') + text, flush=True)
    if not cond:
        FAILS.append(text)


def raises(fn, what):
    try:
        fn()
    except (B.BadInput, BL.Refused) as e:
        return str(e)
    return None


LEDGER = '''scene,side,kind,lane,lo,hi,unit,source,when,note
bryan,gpu,baseline,,24.2,24.2,ms,bench y,2026-11-14 00:20,
bryan,fps,baseline,,25.4,25.4,fps,bench y,2026-11-14 00:20,
bryan,fps,target,,60,60,fps,plan,2026-11-14 00:20,
bryan,gpu,measured,lane_c,-2.0,-2.0,ms,by hand,2026-11-14 01:00,
'''
LAB = 'lane,scene,side,ms,when\nlane_a,bryan,gpu,-3.1,2026-11-14 02:00\nlane_a,bryan,gpu,-2.6,2026-11-14 03:00\n'
TRAINS_LONG = ('lane,stage,when\nlane_a,started,2026-11-14 01:00\nlane_a,handback,2026-11-14 02:00\nlane_a,landed_session,2026-11-14 02:30\n'
               'lane_b,started,2026-11-14 01:00\nlane_b,handback,2026-11-14 03:00\n')
TRAINS_WIDE = ('lane,started,handback,landed_session\nlane_a,2026-11-14 01:00,2026-11-14 02:00,2026-11-14 02:30\n'
               'lane_b,2026-11-14 01:00,2026-11-14 03:00,\n')


def make_inputs(d):
    d = Path(d)
    (d / 'lanes').mkdir()
    (d / 'ledger.csv').write_text(LEDGER, encoding='utf-8')
    (d / 'lab.csv').write_text(LAB, encoding='utf-8')
    (d / 'trains.csv').write_text(TRAINS_LONG, encoding='utf-8')
    for lane, claim in (('lane_a', {'scene': 'bryan', 'side': 'gpu', 'ms': [-4, -2]}), ('lane_b', {'scene': 'bryan', 'side': 'gpu', 'ms': [-1, 0]}),
                        ('lane_c', {'scene': 'bryan', 'side': 'gpu', 'ms': [-3, -1]})):
        (d / 'lanes' / f'{lane}.json').write_text(json.dumps({'claim': claim, 'checks': []}), encoding='utf-8')
    (d / 'lanes' / 'nolane.json').write_text(json.dumps({'checks': []}), encoding='utf-8')


def board(d, **over):
    d = Path(d)
    ledger = B.read_ledger(over.get('ledger', d / 'ledger.csv'))
    claims = B.read_claims(d / 'lanes')
    lab, lp = B.read_lab(over.get('lab', d / 'lab.csv'), claims)
    trains = B.read_trains(over.get('trains', d / 'trains.csv'))
    return B.render(ledger, claims, lab, lp, trains)


def run():
    tmp = Path(tempfile.mkdtemp(prefix='budget_selftest_'))
    try:
        # ---- the board
        make_inputs(tmp)
        text = board(tmp)
        ok('lane `lane_a`' in text and '**+-2.6**'.replace('+-', '-') in text.replace('+-', '-') and 'measured' in text, 'lab row: the newest row per lane/scene/side wins (-2.6, not -3.1)')
        ok('-3.1' not in text, 'the older lab row (-3.1) is not shown')
        ok('claim -4.0..-2.0 / **-2.6** measured' in text.replace('+-', '-'), 'a lane row shows its claim range and its measurement')
        ok('claim -1.0..0.0 / not measured' in text, 'planted twin: a claim without a measurement reads "not measured"')
        ok('- `lane_b`: bryan / GPU ms' in text and '- `lane_a`' not in text.split('## Lanes without a measured row')[1].split('##')[0],
           'the not-done list names lane_b and not the measured lane_a')
        ok('claim -3.0..-1.0 / **-2.0** measured' in text, 'a hand-entered ledger "measured" row counts as a measurement')
        ok('| baseline | 24.2 |' in text and '| target |' in text, 'baseline and target rows come from the ledger')
        ok('| started | handback | 2 | 90 | 120 |' in text, 'trains: waiting time per stage (median / max minutes)')
        ok('Lanes running (started, no hand-back): 0' in text and 'Lanes waiting (handed back, not landed on the session branch): 1 (`lane_b`)' in text,
           'trains: lanes running / waiting')
        (tmp / 'trains_wide.csv').write_text(TRAINS_WIDE, encoding='utf-8')
        ok(board(tmp, trains=tmp / 'trains_wide.csv') == text, 'trains: the wide form gives the same board as the long form')
        no_lab = board(tmp, lab=tmp / 'nope.csv')
        ok('No Surface lab results yet' in no_lab and 'claim -4.0..-2.0 / not measured' in no_lab, 'no lab file: every lane reads not measured, and the board says why')
        # ---- planted faults in the inputs: each must be refused loudly
        bad = tmp / 'bad'
        bad.mkdir()
        (bad / 'lab_nocol.csv').write_text('lane,scene,side\nlane_a,bryan,gpu\n', encoding='utf-8')
        ok(raises(lambda: board(tmp, lab=bad / 'lab_nocol.csv'), 'x') and 'neither' in raises(lambda: board(tmp, lab=bad / 'lab_nocol.csv'), 'x'),
           'planted: a lab file without the ms column is refused')
        (bad / 'lab_nan.csv').write_text('lane,scene,side,ms\nlane_a,bryan,gpu,fast\n', encoding='utf-8')
        ok(raises(lambda: board(tmp, lab=bad / 'lab_nan.csv'), 'x'), 'planted: a non-number in the lab file is refused')
        (bad / 'led_scene.csv').write_text(LEDGER.replace('bryan,gpu,baseline', 'bryn,gpu,baseline'), encoding='utf-8')
        ok(raises(lambda: board(tmp, ledger=bad / 'led_scene.csv'), 'x'), 'planted: an unknown scene in the ledger is refused')
        (bad / 'led_extra.csv').write_text(LEDGER.replace('bench y,2026-11-14 00:20,\nbryan,fps,baseline', 'bench y, with comma,2026-11-14 00:20,\nbryan,fps,baseline'), encoding='utf-8')
        ok(raises(lambda: board(tmp, ledger=bad / 'led_extra.csv'), 'x'), 'planted: a ledger row with an extra field (a comma in a text) is refused')
        (bad / 'led_unit.csv').write_text(LEDGER.replace('24.2,24.2,ms', '24.2,24.2,fps', 1), encoding='utf-8')
        ok(raises(lambda: board(tmp, ledger=bad / 'led_unit.csv'), 'x'), 'planted: a unit that does not match the side is refused')
        (bad / 'tr.csv').write_text('lane,stage,when\nlane_a,napping,2026-11-14 01:00\n', encoding='utf-8')
        ok(raises(lambda: board(tmp, trains=bad / 'tr.csv'), 'x'), 'planted: an unknown trains stage is refused')
        (tmp / 'lanes' / 'lane_d.json').write_text(json.dumps({'claim': {'scene': 'bryan', 'side': 'gpu', 'ms': [-1]}, 'checks': []}), encoding='utf-8')
        ok(raises(lambda: board(tmp), 'x'), 'planted: a claim whose "ms" is not [lo, hi] is refused')
        (tmp / 'lanes' / 'lane_d.json').unlink()
        # ---- the lab's own file (M2 job rows; header = tools\surface\lab.py FIELDS)
        m2h = ','.join(['job_id', 'finished', 'host', 'exe_sha', 'capture', 'switches', 'label', 'frames', 'captured_frames', 'repeat', 'warm_frames', 'status', 'error',
                        'exit_code', 'timed_frames', 'first_frame_ms', 'render_ms_med', 'render_ms_p99', 'bind_ms_med', 'gpu_ms_med', 'gpu_ms_p99', 'gpu_wait_ms_med',
                        'drift_pct', 'run_gpu_ms_med', 'passes_ms', 'wall_s']) + '\n'

        def row(jid, cap, sw, st='ok', rms=3.0, gms=10.0, **kw):
            v = dict(finished='2026-11-14 01:00:00', host='surface', exe_sha='abc123def456', frames='100', captured_frames='120', repeat='2', warm_frames='30', drift='1.0')
            v.update(kw)
            return (f"{jid},{v['finished']},{v['host']},{v['exe_sha']},{cap},{sw},x,{v['frames']},{v['captured_frames']},{v['repeat']},{v['warm_frames']},{st},,0,4,12.8,"
                    f"{rms},3.5,1.2,{gms},8.8,9.4,{v['drift']},8.4/8.8,,24.8\n")
        lane_e = tmp / 'lanes' / 'lane_e.json'
        lane_e.write_text(json.dumps({'claim': {'scene': 'bryan', 'side': 'gpu', 'ms': [-3, -1], 'switch': 'wwe2k14_rdr_e'}, 'checks': []}))

        def lab_text(*rows_, header=m2h):
            return header + ''.join(rows_)

        def cell(*rows_, header=m2h):
            (tmp / 'lab_m2.csv').write_text(lab_text(*rows_, header=header), encoding='utf-8')
            return board(tmp, lab=tmp / 'lab_m2.csv')
        BASE = row('j1', 's100_bryan_entrance', '', gms=10.0)
        WITH = row('j2', 's100_bryan_entrance', 'wwe2k14_rdr_e=true', gms=7.5)
        t2 = cell(BASE, WITH, row('j3', 's100_match300', 'wwe2k14_rdr_e=true', gms=1.0))
        ok('/ **-2.5** measured' in t2 and 'lane `lane_e`' in t2, 'lab job rows: measurement = job with the switch minus the same capture without it (-2.5), on the claim\'s scene only')
        nm = lambda t: '- `lane_e`: bryan / GPU ms' in t
        ok(nm(cell(WITH)), 'planted: a switch job with no baseline job on the same capture stays "not measured"')
        ok(nm(cell(BASE, row('j2', 's100_bryan_entrance', 'wwe2k14_rdr_e=true', st='failed', rms='', gms=''))), 'planted: a failed job is not a measurement (never 0 ms)')
        ok(raises(lambda: cell(BASE, header=m2h.replace('gpu_ms_med', 'gpu_ms_x')), 'x'), 'planted: job rows without the gpu_ms_med column are refused')
        # review item 3: switch VALUES. Only true / nonzero is on.
        ok(nm(cell(BASE, row('j2', 's100_bryan_entrance', 'wwe2k14_rdr_e=false', gms=7.5))), 'a job with the claim\'s switch =false is NOT a "with" job')
        ok(nm(cell(BASE, row('j2', 's100_bryan_entrance', 'wwe2k14_rdr_e=0', gms=7.5))), 'a job with the claim\'s switch =0 is not a "with" job')
        ok('**-2.5** measured' in cell(row('j1', 's100_bryan_entrance', 'wwe2k14_rdr_e=false', gms=10.0), WITH), 'a baseline spelled k=false is a valid baseline (false == absent)')
        ok('**-2.5** measured' in cell(BASE, row('j2', 's100_bryan_entrance', 'wwe2k14_rdr_e=1', gms=7.5)), 'k=1 is on')
        # base switches must equal the measured job's minus the claim's
        ok(nm(cell(row('j1', 's100_bryan_entrance', 'other=true', gms=10.0), WITH)), 'planted: a baseline with an extra OTHER switch is not the baseline (not measured)')
        ok('**-2.5** measured' in cell(row('j1', 's100_bryan_entrance', 'other=true', gms=10.0), row('j2', 's100_bryan_entrance', 'other=true wwe2k14_rdr_e=true', gms=7.5)),
           'a baseline with the same other switches IS the baseline')
        # comparable: one planted test per field
        for fld, bad, why in (('exe_sha', 'ffffffffffff', 'exe_sha'), ('host', 'pc', 'host'), ('frames', '200', 'frames'), ('captured_frames', '90', 'captured_frames'),
                              ('warm_frames', '10', 'warm_frames'), ('repeat', '3', 'repeat'), ('drift', '3.5', 'drift_pct'), ('drift', '-4', 'drift_pct'),
                              ('finished', '2026-11-16 01:00:00', 'finished')):
            t = cell(BASE, row('j2', 's100_bryan_entrance', 'wwe2k14_rdr_e=true', gms=7.5, **{fld: bad}))
            ok(f'not comparable: {why}' in t.replace('differs', 'differs') and nm(t) and '**-2.5**' not in t, f'planted: {fld} differs/out of band -> "not comparable: {why}"')
        ok('not comparable: drift_pct not recorded' in cell(BASE, row('j2', 's100_bryan_entrance', 'wwe2k14_rdr_e=true', gms=7.5, drift='')),
           'planted: a job with no drift_pct (repeat 1) is not comparable')
        # capture -> scene: an explicit column, else a fixed prefix
        ok(nm(cell(row('j1', 'xbryan_foo', '', gms=10.0), row('j2', 'xbryan_foo', 'wwe2k14_rdr_e=true', gms=7.5))), 'a capture named xbryan_foo is not a bryan capture')
        ok(nm(cell(row('j1', 'brock_vs_bryan', '', gms=10.0), row('j2', 'brock_vs_bryan', 'wwe2k14_rdr_e=true', gms=7.5))), 'brock_vs_bryan is a BROCK capture (prefix), not bryan')
        ok('**-2.5** measured' in cell(row('j1', 'bryan_entrance', '', gms=10.0), row('j2', 'bryan_entrance', 'wwe2k14_rdr_e=true', gms=7.5)), 'bryan_entrance (no sNNN_) is bryan')
        sc_h = m2h.replace('job_id,', 'job_id,scene,', 1)
        sc_row = lambda r_, sc_: r_.replace(r_.split(',')[0] + ',', r_.split(',')[0] + f',{sc_},', 1)
        ok('**-2.5** measured' in cell(sc_row(row('j1', 'weird_name', '', gms=10.0), 'bryan'), sc_row(row('j2', 'weird_name', 'wwe2k14_rdr_e=true', gms=7.5), 'bryan'), header=sc_h),
           'an explicit scene column decides the scene')
        # the newest full bench: pick the newest bench, then its sides
        old = 'bryan,gpu,bench,,20.0,20.0,ms,bench old,2026-11-14 01:00,\nbryan,fps,bench,,30.0,30.0,fps,bench old,2026-11-14 01:00,\n'
        new = 'bryan,fps,bench,,40.0,40.0,fps,bench new,2026-11-14 02:00,\n'
        (tmp / 'ledger_b.csv').write_text(LEDGER + old + new, encoding='utf-8')
        tb = board(tmp, ledger=tmp / 'ledger_b.csv')
        brow = [l for l in tb.split('## Bryan')[1].splitlines() if l.startswith('| newest full bench')][0]
        ok('40.0' in brow and '20.0' not in brow and '30.0' not in brow, f'planted: the newest bench\'s sides only; an older bench\'s GPU is not mixed in ({brow[:80]})')
        lane_e.unlink()
        # ---- the real ledger and the real lane files parse
        led = B.read_ledger(B.REPO / 'docs' / 'renderer' / 'BUDGET_LEDGER.csv')
        ok(len(led) > 0 and any(r['kind'] == 'target' for r in led) and not any(r['kind'] == 'baseline' and 'bench_line' in r['note'] for r in led), f'the committed ledger parses ({len(led)} rows; no seeded baselines, S0-5 owns them)')
        B.read_claims(B.REPO / 'tools' / 'gate' / 'lanes')
        ok(True, 'every committed lane json parses (claims read)')
        # ---- the one-line bench (more in tools\board\bench_line_selftest.py, run below)
        import bench_line_selftest
        if bench_line_selftest.run(tmp, ok, B, BL, FIX) != 0:
            pass
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print(f'budget selftest: {"FAIL, " + str(len(FAILS)) + " check(s)" if FAILS else "all checks hold"}')
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(run())
