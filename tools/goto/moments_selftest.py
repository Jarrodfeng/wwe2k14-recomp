"""S101 lane 31: the moments self test (gate entry tools/gate/lanes/moments.json). No game, no game data: every fixture is written here.

    py -I tools/goto/moments_selftest.py

1. moment: addresses round-trip; malformed ones are refused.
2. A synthetic recording in src/replay.h's format (and the committed tools/xdet/refs/xdet_cena_taker_a.rec) parse; end_frame is the game's rule.
3. The library: add / list / check on a temp folder (RDR_MOMENTS_DIR); a bad recording is refused; a changed recording is caught.
4. Dry-run plans: @frame:F inside the recording -> a plan; past the end, frame 0, an event the recording lacks, an event index past the
   count, an unknown moment -> ERROR (never a plan).
5. Re-finds a real past bug: Session 77's input removal (xdet_cena_taker_pad_nowalk2679: the walk at S 2679 removed, DIVERGED at frame 2685, SIM
   c0d + LAYOUT c0; SESSION77_SUMMARY.md "Sensitivity controls", XDET_PLAN.md ~229): a fixture hash stream that diverges at 2685 in c0d / c0
   makes the track check stop AT 2685 and name the columns; the same stream checked up to 2684 passes; the twin without the fault passes.
6. The track check never passes silently: a short run stream, a short reference, no shared columns, a headerless file all stop / refuse.
7. The way back from a case manifest: session.rec -> a moment: address that resolves; builder only -> not frame-exact; nothing -> refused.
8. The game side (src/replay.h) is read: the cvars, one hand-over block shared by the end and replay_until, events dropped after F.
Exit 0 = all passed.
"""
import json
import os
import re
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import address  # noqa: E402
import moments  # noqa: E402
import resolve  # noqa: E402

problems = []


def check(ok, what):
    if not ok:
        problems.append(what)
        print('FAIL: ' + what)
    return ok


def write(path, text):
    with open(path, 'w', encoding='utf-8', newline='') as f:
        f.write(text)


def make_rec(end=3000, events=((1200, 'runin target=1 char=-1 second=-1 force=1'), (1800, 'director cut2'), (2400, 'runin target=2 char=-1 second=-1 force=1')),
             seed=77, header=True):
    out = ['# wwe2k14 replay v2 (src/replay.h): synthetic fixture of tools/goto/moments_selftest.py'] if header else []
    out += ['name fx', 'seed %d' % seed, 'dlc none', 'flag det_frames true', 'flag det_serial true']
    ev = {}
    for f, t in events:
        ev.setdefault(f, []).append(t)
    for f in range(1, end + 1):
        if f % 60 == 0:
            out.append('T %d %d' % (f, f * 3))
        if f % 97 == 1:
            out.append('S %d 0 0 0 0 00000000%08X' % (f, f))
        for t in ev.get(f, []):
            out.append('E %d %s' % (f, t))
    return '\n'.join(out) + '\n'


def make_det(frames=3000, flip_at=0, cols=('vb', 'c0', 'c0d', 'p0', 'sim1'), flip_cols=('c0', 'c0d'), bad_cell_at=0):
    out = ['# det_hash exe=C:\\fixture\\wwe2k14.exe size=1 mtime=x spec=default,objects,sim', '# xdet platform=fixture',
           'frame,' + ','.join(cols)]
    for f in range(1, frames + 1):
        cells = []
        for c in cols:
            v = '%016x' % ((f * 2654435761 + hash_name(c)) & 0xFFFFFFFFFFFFFFFF)
            if flip_at and f >= flip_at and c in flip_cols:
                v = '%016x' % (int(v, 16) ^ 1)
            if bad_cell_at and f == bad_cell_at and c == 'p0':
                v = '!'
            cells.append(v)
        out.append('%d,%s' % (f, ','.join(cells)))
    return '\n'.join(out) + '\n'


def hash_name(s):
    h = 1469598103934665603
    for ch in s:
        h = ((h ^ ord(ch)) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def main():
    tmp = tempfile.mkdtemp(prefix='moments_selftest_')
    lib = os.path.join(tmp, 'lib')
    os.environ['RDR_MOMENTS_DIR'] = lib

    # 1. addresses
    for a in ['moment:fx@frame:300', 'moment:fx@event:runin', 'moment:fx@event:runin:2', 'moment:a.b-c_1@frame:1']:
        try:
            check(str(address.parse(a)) == a, 'round trip %s -> %s' % (a, address.parse(a)))
        except address.AddressError as x:
            check(False, 'valid %s refused: %s' % (a, x))
    check(str(address.parse('moment:fx@event:runin:0')) == 'moment:fx@event:runin', 'event :0 prints without the index')
    for a in ['moment:', 'moment:fx', 'moment:fx@', 'moment:fx@frame:', 'moment:fx@frame:-1', 'moment:fx@frame:abc', 'moment:fx@event:',
              'moment:fx@event:a b', 'moment:fx@bogus', 'moment: @frame:3', 'moment:f/x@frame:3', 'moment:fx@frame:1234567890']:
        try:
            address.parse(a)
            check(False, 'malformed %r was accepted' % a)
        except address.AddressError:
            pass

    # 2. the recording parser
    rec_path = os.path.join(tmp, 'fx_src.rec')
    write(rec_path, make_rec())
    rec = moments.parse_rec(rec_path)
    check(rec.problems == [], 'synthetic rec has problems: %s' % rec.problems[:3])
    check(rec.end_frame == 3000 and rec.seed == 77 and len(rec.events) == 3 and rec.version == 2,
          'parse: end %d seed %d events %d version %d' % (rec.end_frame, rec.seed, len(rec.events), rec.version))
    real = os.path.join(REPO, 'tools', 'xdet', 'refs', 'xdet_cena_taker_a.rec')
    if check(os.path.isfile(real), 'the committed reference recording is missing: ' + real):
        r = moments.parse_rec(real)
        check(r.end_frame > 1000 and not r.problems, 'committed xdet_cena_taker_a.rec: end %d problems %s' % (r.end_frame, r.problems[:3]))
    try:
        moments.parse_rec(os.path.join(tmp, 'nope.rec'))
        check(False, 'a missing recording did not raise')
    except moments.MomentError:
        pass
    write(os.path.join(tmp, 'empty.rec'), '')
    check(bool(moments.parse_rec(os.path.join(tmp, 'empty.rec')).problems), 'an empty recording has no problems')
    write(os.path.join(tmp, 'garbage.rec'), 'hello\nS x y z\nT 60\n')
    check(len(moments.parse_rec(os.path.join(tmp, 'garbage.rec')).problems) >= 3, 'garbage lines were not all reported')

    # 3. the library
    det_ok = os.path.join(tmp, 'det_ok.csv')
    write(det_ok, make_det())
    meta = moments.add_moment('fx', rec_path, det_ok, shows='fixture scene', build='selftest')
    check(meta['frames'] == 3000 and meta['events'] == 3, 'moment.json counts')
    check(os.path.isfile(os.path.join(lib, 'KEEP')), 'no KEEP file in the library')
    check(moments.list_moments() == ['fx'], 'list: %s' % moments.list_moments())
    check(moments.check_moment('fx') == [], 'check fx: %s' % moments.check_moment('fx'))
    for bad_name, bad_file in [('fx', rec_path), ('bad', os.path.join(tmp, 'garbage.rec')), ('bad2', os.path.join(tmp, 'empty.rec')),
                               ('b/d', rec_path), ('bad3', os.path.join(tmp, 'nope.rec'))]:
        try:
            moments.add_moment(bad_name, bad_file)
            check(False, 'add %s %s was accepted' % (bad_name, os.path.basename(bad_file)))
        except moments.MomentError:
            pass
    check(moments.list_moments() == ['fx'], 'a refused add left something behind: %s' % moments.list_moments())
    # a hash stream that stops short of the recording is a check problem
    det_short = os.path.join(tmp, 'det_short.csv')
    write(det_short, make_det(frames=1000))
    moments.add_moment('short', rec_path, det_short)
    check(any('hash stream ends' in p for p in moments.check_moment('short')), 'a short hash stream passed check')
    # a changed recording is caught (positive control for the sha)
    with open(os.path.join(lib, 'fx', 'fx.rec'), 'a', newline='') as f:
        f.write('E 3000 sneaked\n')
    check(any('changed' in p for p in moments.check_moment('fx')), 'a changed recording passed check')
    with open(os.path.join(lib, 'fx', 'fx.rec'), 'r', newline='') as f:
        txt = f.read()
    write(os.path.join(lib, 'fx', 'fx.rec'), txt.replace('E 3000 sneaked\n', ''))
    check(moments.check_moment('fx') == [], 'restored fx does not check: %s' % moments.check_moment('fx'))
    ctx = resolve.Context(atlas=object(), scex={'eras': {}, 'matches': {}})

    # 4. dry-run plans
    def plan(a):
        return resolve.resolve(a, ctx)

    p = plan('moment:fx@frame:2999')
    check(p.status == 'plan' and [s['op'] for s in p.steps] == ['moment_start', 'moment_wait'] and p.steps[0]['until'] == 2999, 'frame 2999: %s %s' % (p.status, p.reason))
    check(plan('moment:fx@frame:3000').status == 'plan', 'the last frame itself must plan')
    p = plan('moment:fx@frame:3001')
    check(p.status == 'error' and 'past' in p.reason, 'frame 3001 past the end: %s %s' % (p.status, p.reason))
    check(plan('moment:fx@frame:0').status == 'error', 'frame 0 must be an error (replay_until=0 means off)')
    p = plan('moment:fx@event:runin')
    check(p.status == 'plan' and p.steps[0]['until'] == 1200, 'first runin event -> 1200: %s %s' % (p.status, p.steps[:1]))
    p = plan('moment:fx@event:runin:1')
    check(p.status == 'plan' and p.steps[0]['until'] == 2400, 'second runin event -> 2400')
    p = plan('moment:fx@event:director')
    check(p.status == 'plan' and p.steps[0]['until'] == 1800, 'director event -> 1800')
    p = plan('moment:fx@event:entrance')
    check(p.status == 'error' and 'no event' in p.reason and 'runin' in p.reason, 'a missing event is an error naming the events: %s' % p.reason)
    p = plan('moment:fx@event:runin:2')
    check(p.status == 'error' and '2 "runin"' in p.reason, 'event index past the count: %s' % p.reason)
    p = plan('moment:nothere@frame:5')
    check(p.status == 'error' and 'no moment' in p.reason, 'unknown moment: %s' % p.reason)
    flags = resolve.moment_flags(plan('moment:fx@frame:2999').steps[0])
    check('--wwe2k14_replay_until=2999' in flags and '--wwe2k14_replay_play=fx' in flags and '--vsync=false' in flags, 'flags: %s' % flags)
    check('--wwe2k14_det_hash=default,objects,sim' in flags, 'the reference stream\'s own spec is not passed: %s' % flags)
    check(len(plan('moment:short@frame:10').notes) >= 2, 'plan notes missing')

    # 5. the real past bug: S77 input removal, DIVERGED at 2685 in c0d + c0
    ref = os.path.join(tmp, 'ref.csv')
    run_bad = os.path.join(tmp, 'run_bad.csv')
    run_ok = os.path.join(tmp, 'run_ok.csv')
    write(ref, make_det())
    write(run_ok, make_det())
    write(run_bad, make_det(flip_at=2685))
    t = moments.track_check(ref, run_ok, 3000)
    check(t.ok and t.compared == 3000, 'the twin without the fault: %s' % t.line())
    t = moments.track_check(ref, run_bad, 3000)
    check((not t.ok) and t.first_slip == 2685 and t.columns == ['c0', 'c0d'], 'the planted slip at 2685 was not found there: %s' % t.line())
    check('2685' in t.line() and 'c0d' in t.line(), 'the stop line does not name the frame and the regions: ' + t.line())
    t = moments.track_check(ref, run_bad, 2684)
    check(t.ok, 'a check up to 2684 must pass (the slip is later): %s' % t.line())
    t = moments.track_check(ref, run_bad, 2685)
    check((not t.ok) and t.first_slip == 2685, 'a check up to 2685 must stop at 2685')
    t = moments.track_check(ref, run_bad, 3000, from_frame=2686)
    check((not t.ok) and t.first_slip == 2686, 'incremental from 2686 must still see the flipped state')
    # the first slip, not a later one: two flips
    write(os.path.join(tmp, 'run_two.csv'), make_det(flip_at=100, flip_cols=('sim1',)))
    t = moments.track_check(ref, os.path.join(tmp, 'run_two.csv'), 3000)
    check(t.first_slip == 100 and t.columns == ['sim1'], 'earliest slip: ' + t.line())

    # 6. never a silent pass
    write(os.path.join(tmp, 'run_short.csv'), make_det(frames=500))
    t = moments.track_check(ref, os.path.join(tmp, 'run_short.csv'), 3000)
    check((not t.ok) and 'ends at frame 500' in t.reason, 'a short run stream passed or was not named: ' + t.line())
    t = moments.track_check(os.path.join(tmp, 'run_short.csv'), run_ok, 3000)
    check((not t.ok) and 'reference' in t.reason, 'a short reference passed: ' + t.line())
    write(os.path.join(tmp, 'other.csv'), make_det(cols=('x1', 'x2')))
    t = moments.track_check(ref, os.path.join(tmp, 'other.csv'), 100)
    check((not t.ok) and 'share no region' in t.reason, 'different specs passed: ' + t.line())
    write(os.path.join(tmp, 'noheader.csv'), '# det_hash only a comment\n')
    for a, b in [(ref, os.path.join(tmp, 'noheader.csv')), (os.path.join(tmp, 'noheader.csv'), ref), (ref, os.path.join(tmp, 'absent.csv'))]:
        try:
            moments.track_check(a, b, 10)
            check(False, 'an unreadable stream did not raise')
        except moments.MomentError:
            pass
    write(os.path.join(tmp, 'bad_cells.csv'), make_det(bad_cell_at=500))
    t = moments.track_check(ref, os.path.join(tmp, 'bad_cells.csv'), 3000)
    check(t.ok and t.unreadable == 1, 'an unreadable cell must be skipped and counted: ' + t.line())

    # 7. the way back from a case zip
    case_dir = os.path.join(tmp, 'case1')
    os.makedirs(case_dir)
    man = {'id': 'case_20261111_1', 'where': {'words': 'fixture scene'}, 'way_back': {'builder': 'match:A vs B', 'inputs': 'inputs.rec', 'first_frame': 2700,
           'moment_frame': 2900, 'seed': 77, 'replay_mode': True}}
    write(os.path.join(case_dir, 'session.rec'), make_rec())
    adr, notes = moments.wayback(man, case_dir)
    check(adr == 'moment:case_case_20261111_1@frame:2900', 'wayback address: ' + adr)
    check(plan(adr).status == 'plan' and plan(adr).steps[0]['until'] == 2900, 'the way-back address does not resolve to a plan')
    man2 = json.loads(json.dumps(man))
    man2['id'] = 'two'
    man2['way_back']['moment_frame'] = 3500
    try:
        moments.wayback(man2, case_dir)
        check(False, 'a moment past session.rec was accepted')
    except moments.MomentError as x:
        check('before the moment' in str(x), 'wrong message: %s' % x)
    man3 = json.loads(json.dumps(man))
    man3['id'] = 'three'
    man3['way_back']['seed'] = 5
    try:
        moments.wayback(man3, case_dir)
        check(False, 'a seed mismatch was accepted')
    except moments.MomentError:
        pass
    empty_case = os.path.join(tmp, 'case2')
    os.makedirs(empty_case)
    adr, notes = moments.wayback(man, empty_case)
    check(adr == 'match:A vs B' and any('not frame-exact' in n and 'session.rec' in n for n in notes), 'builder-only way back: %s %s' % (adr, notes))
    man4 = json.loads(json.dumps(man))
    man4['way_back']['builder'] = None
    try:
        moments.wayback(man4, empty_case)
        check(False, 'a way back with nothing was accepted')
    except moments.MomentError as x:
        check('inputs.rec holds only the window' in str(x), 'wrong message: %s' % x)

    # 7b. review fixes (lane 31 review): each check below failed on the first version (4956c0d4)
    # (1) a hash stream being written: the last line without a newline is cut off, and is dropped, not judged
    full = make_det(frames=200)
    cut = full[:full.rindex('\n') - 7]   # the last row loses its newline and its last 7 chars: "200,...,<9 of 16 hex digits>"
    write(os.path.join(tmp, 'run_cut.csv'), cut)
    cols_c, rows_c, _ = moments.parse_det(os.path.join(tmp, 'run_cut.csv'))
    check(max(rows_c) == 199, 'parse_det kept the cut-off last row (frames up to %d)' % max(rows_c))
    t = moments.track_check(ref, os.path.join(tmp, 'run_cut.csv'), 199)
    check(t.ok and t.compared == 199, 'a stream cut mid-row must still pass up to its last whole row: ' + t.line())
    t = moments.track_check(ref, os.path.join(tmp, 'run_cut.csv'), 200)
    check((not t.ok) and 'ends at frame 199' in t.reason and t.columns == [], 'the cut row must be "stream too short", not a false slip: ' + t.line())
    bl = make_det(frames=50).split('\n')
    bl[10] = bl[10].rsplit(',', 1)[0] + ',zz'   # a whole row whose last cell is not a hash
    write(os.path.join(tmp, 'run_badcell.csv'), '\n'.join(bl))
    try:
        moments.parse_det(os.path.join(tmp, 'run_badcell.csv'))
        check(False, 'a non-hex cell in a whole row was accepted')
    except moments.MomentError:
        pass
    # add --det with a cut-off last line is fine (the line is dropped), a headerless one still refused
    moments.add_moment('cutdet', rec_path, os.path.join(tmp, 'run_cut.csv'))
    # (7) the library: sha required for rec and det; an event at frame 0; the reference must reach the frame at plan time
    with open(os.path.join(lib, 'cutdet', 'moment.json')) as f:
        mj = json.load(f)
    del mj['rec_sha256']
    write(os.path.join(lib, 'cutdet', 'moment.json'), json.dumps(mj))
    try:
        moments.load_moment('cutdet')
        check(False, 'a moment.json without rec_sha256 was accepted')
    except moments.MomentError as x:
        check('rec_sha256' in str(x), 'wrong message: %s' % x)
    moments.add_moment('dettamper', rec_path, det_ok)
    with open(os.path.join(lib, 'dettamper', 'det.csv'), 'a', newline='') as f:
        f.write('\n')
    try:
        moments.load_moment('dettamper')
        check(False, 'a changed det.csv was accepted')
    except moments.MomentError as x:
        check('hash stream changed' in str(x), 'wrong message: %s' % x)
    p = plan('moment:short@frame:2000')
    check(p.status == 'error' and 'hash stream ends at frame 1000' in p.reason, 'a frame the reference does not reach must fail at plan time: %s %s' % (p.status, p.reason))
    write(os.path.join(tmp, 'ev0.rec'), make_rec(events=((5, 'x'),)) + 'E 0 runin target=1\n')
    moments.add_moment('ev0', os.path.join(tmp, 'ev0.rec'))
    p = plan('moment:ev0@event:runin')
    check(p.status == 'error' and 'frame 0' in p.reason, 'an event at frame 0 returned a frame: %s %s' % (p.status, p.reason))
    # (5) the way back: a cut session.rec needs the "E F case" line; a live copy may end mid-line; same save / build / mods
    cutcase = os.path.join(tmp, 'case3')
    os.makedirs(cutcase)
    write(os.path.join(cutcase, 'session.rec'), make_rec(end=2880))
    man5 = json.loads(json.dumps(man))
    man5['id'] = 'five'
    man5['build'] = {'exe_size': 1}
    try:
        moments.wayback(man5, cutcase)
        check(False, 'a session.rec that ends before the moment was accepted')
    except moments.MomentError as x:
        check('RecordEvent' in str(x) and '2880' in str(x), 'the refusal does not say how to fix it: %s' % x)
    write(os.path.join(cutcase, 'session.rec'), make_rec(end=2880) + 'E 2900 case\nS 29')   # marked, then cut mid-line
    adr, notes = moments.wayback(man5, cutcase, data='userdata_frozen', current_build={'exe_size': 2})
    check(adr == 'moment:case_five@frame:2900', 'marked + cut session.rec: ' + adr)
    check(any('same save data' in n for n in notes) and any('another build' in n for n in notes), 'notes lack save/build/mods wording: %s' % notes)
    check(moments.load_moment('case_five')[0].get('data') == 'userdata_frozen', 'the save copy name is not kept in moment.json')

    # (2, 3, 7) the executor, against a fake live channel (no game): liveness, log stops, split lines, vsync back, partial hash rows
    resolve.POLL_S = 0.01
    sys.path.insert(0, os.path.join(REPO, 'tools', 'live'))

    class FakeLive:
        class NoGame(Exception):
            pass

        def __init__(self, base, script):
            self.REPO, self.EXE_DIR, self.script, self.n = base, base, script, 0
            self.log = os.path.join(base, 'run.log')
            self.batches, self.quit, self.gone, self.bm = [], False, False, 0
            write(self.log, '')
            os.makedirs(os.path.join(base, 'out', 'harness', 'determinism'), exist_ok=True)
            self.csv = os.path.join(base, 'out', 'harness', 'determinism', 'det_007.csv')

        def parse(self, text):
            return {'run': '7'}, ''

        def run_log(self, run):
            return self.log

        def append(self, text):
            with open(self.log, 'a', newline='') as f:
                f.write(text)

        def request(self, name, text, connect_wait=1.0):
            if text == 'quit':
                self.quit = True
                return ''
            if self.gone:
                raise self.NoGame('gone')
            self.n += 1
            if self.script:
                self.script(self, self.n)
            return 'ok'

        def run_batch(self, name, lines, timeout=120, **k):
            self.batches.append(lines)
            return {}, '', []

        def bookmark(self, name):
            self.bm += 1
            return 'bookmark taken', None

    INIT = '[wwe2k14][replay] replay_until: hand over at frame 100 (recording ends at 3000)\n[wwe2k14][replay] playing x.rec (1 lines)\n'
    HAND = '[wwe2k14][replay] recording stopped (replay_until) at frame 100: the script / real controller take over.\n'

    def run_exec(script, det_ref=ref, timeout_s=20):
        d = tempfile.mkdtemp(prefix='fakelive_', dir=tmp)
        fl = FakeLive(d, script)
        sys.modules['live'] = fl
        try:
            resolve._moment_wait({'name': 'fx', 'until': 100, 'det': det_ref, 'timeout_s': timeout_s}, 'main', lambda *_: None)
            return fl, None
        except resolve.Stopped as x:
            return fl, str(x)
        finally:
            del sys.modules['live']

    def ok_script(fl, n):
        if n == 1:
            fl.append(INIT)
            write(fl.csv, make_det(frames=60))
        if n == 3:
            write(fl.csv, make_det(frames=101))
            fl.append(HAND)

    fl, err = run_exec(ok_script)
    check(err is None and fl.batches == [['cvar vsync true']], 'good run: stop %s, batches %s' % (err, fl.batches))
    check(not fl.quit, 'a successful goto must leave the game running')

    def partial_script(fl, n):   # at the hand-over the run's stream ends mid-row at F; the row completes a few polls later
        if n == 1:
            fl.append(INIT)
        if n == 2:
            full100 = make_det(frames=100)
            write(fl.csv, full100[:full100.rindex('\n') - 5])
            fl.append(HAND)
        if n == 5:
            write(fl.csv, make_det(frames=101))
    fl, err = run_exec(partial_script)
    check(err is None, 'a half-written row at F made the goto stop (or pass wrongly): %s' % err)

    def never_script(fl, n):
        if n == 1:
            fl.append(INIT)
            fl.append(HAND)
            write(fl.csv, make_det(frames=50))
    resolve.FINAL_WAIT_S = 0.3
    fl, err = run_exec(never_script, timeout_s=5)
    check(err is not None and 'never reached frame 100' in err and fl.quit, 'a stream that never reaches F must stop, not pass: %s' % err)

    def slip_script(fl, n):
        if n == 1:
            fl.append(INIT)
            write(fl.csv, make_det(frames=90, flip_at=61))
    fl, err = run_exec(slip_script)
    check(err is not None and 'frame 61' in err and fl.quit and fl.bm == 1, 'a hash slip at 61 (quit %s, bookmarks %d): %s' % (fl.quit, fl.bm, err))

    def split_script(fl, n):
        if n == 1:
            fl.append(INIT)
            fl.append('[wwe2k14][replay] SLI')
        if n == 3:
            fl.append('P at frame 70: 5 controller calls so far, the recording had 6\n')
    fl, err = run_exec(split_script, det_ref=None)
    check(err is not None and 'slipped' in err and fl.quit, 'a SLIP line split across two reads was missed: %s' % err)

    def gone_script(fl, n):
        if n == 1:
            fl.append(INIT)
        if n == 3:
            fl.gone = True
    fl, err = run_exec(gone_script, det_ref=None)
    check(err is not None and 'stopped answering' in err, 'a vanished game must stop the wait at once: %s' % err)

    def failed_script(fl, n):
        if n == 1:
            fl.append('[wwe2k14][replay] FAILED to read C:\\x\\fx.rec: playback off (replay mode stays on)\n')
    fl, err = run_exec(failed_script, det_ref=None)
    check(err is not None and 'does not play' in err and fl.quit, 'FAILED to read must stop: %s' % err)

    def noinit_script(fl, n):
        if n == 1:
            fl.append('[wwe2k14][replay] playing x.rec (1 lines)\n')
    fl, err = run_exec(noinit_script, det_ref=None)
    check(err is not None and 'ignores' in err, 'a playback without the Init line (an exe without the switch) must stop: %s' % err)

    def ended_script(fl, n):
        if n == 1:
            fl.append(INIT + '[wwe2k14][replay] recording ended at frame 3000: the script / real controller take over.\n')
    fl, err = run_exec(ended_script, det_ref=None)
    check(err is not None and 'ended before' in err, 'a recording that ended instead of stopping must stop the goto: %s' % err)

    # 8. the game side, read
    h = open(os.path.join(REPO, 'src', 'replay.h'), encoding='utf-8').read().replace('\r\n', '\n')
    for need in ['REXCVAR_DEFINE_INT32(wwe2k14_replay_until', 'REXCVAR_DEFINE_STRING(wwe2k14_replay_hold', 'S101 lane31', 'SetupUntil();',
                 'frame >= stop_frame_', 'frame <= event_limit_', 'recording {} at frame {}']:
        check(need in h, 'src/replay.h lacks: ' + need)
    onframe = h[h.index('void OnFrameLocked'):h.index('// Wrappers around the game')]
    check(onframe.count('playing_ = false;') == 1 and 'frame >= end_frame_' not in onframe,
          'the end and replay_until must share one hand-over block (found %d, end_frame_ test left: %s)' % (onframe.count('playing_ = false;'), 'frame >= end_frame_' in onframe))
    check('stop_frame_ = end_frame_;' in h, 'stop_frame_ does not default to the recording end')
    # review: the hold runs after the frame's hash and the swap, not inside OnFrame; the unused-switch warning; the recorder accessor
    check('if (until_active_) HoldAfterUntil(frame);  // S101 lane31\n  }' not in h and 'void HoldPoint(uint64_t frame)' in h and
          h.count('HoldAfterUntil(frame)') == 1, 'the hold must not be called from OnFrame')
    check('given without replay_play' in h and 'std::filesystem::path RecorderPath()' in h, 'Init warning / RecorderPath missing')
    ko = open(os.path.join(REPO, 'src', 'kernel_overrides.h'), encoding='utf-8').read()
    i_hash, i_entry, i_after, i_hold = (ko.find(x) for x in ('FrameHashOnFrame(g_guest_frame)', 'VdSwap_entry(buffer_ptr, fetch_ptr',
                                                             'd3d_noring::AfterVdSwap(', 'g_replay.HoldPoint(g_guest_frame)'))
    check(0 < i_hash < i_entry < i_after < i_hold and 'g_replay.OnFrame(g_guest_frame);' in ko and ko.count('HoldPoint') == 1,
          'HoldPoint must come after FrameHashOnFrame, VdSwap_entry and AfterVdSwap (offsets %s)' % [i_hash, i_entry, i_after, i_hold])
    rs = open(os.path.join(HERE, 'resolve.py'), encoding='utf-8').read()
    for need in ['cvar vsync true', '--audio_mute=true', '_moment_quit', 'FAILED to read', 'C.load_cvars()', '_exe_knows', 'carry = lines.pop()']:
        check(need in rs, 'tools/goto/resolve.py lacks: ' + need)

    print('moments selftest: %s' % ('%d problems' % len(problems) if problems else '0 problems (all checks passed)'))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
