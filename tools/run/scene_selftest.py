r"""S109 SCENE selftest (no game, no window): `py -I tools\run\scene_selftest.py [--scene PATH]`

1. Six golden lowerings: a scene request lowered by tools\run\scene.py must give, step by step, the semantic lines of the S108 route it replaces
   (tools\input_scripts\scenes\s108_*.txt): bld7_bloodon (builder + bleeds + face shots), shot_band, hair_match (the fixed `103::2`), fandango_entrance,
   caa_match, ban_arena53. Semantic = the route's steps without comments, `log`, `mark`, `include`, `on_fail`, the match_go number and the closing wait + quit;
   the launch flags are compared as sets. The two deliberate differences (fandango: `every` instead of 14 hand-copied shots, BG04 wait not written) are stated below.
2. Planted refusals: each must raise Refused with its reason (a stub that never refuses fails every one of them: `--scene STUB.py`).
3. Every generated route passes tools\bench\route_lint.py with 0 ERROR fast-forwarded (no watch / performance term).
4. Routes file on the coverage board under their item word.
5. S110 SCENEMODES (docs\research\s110\SCENEMODES.md): today's match lowerings pinned byte-identical (sha1 of the whole route); per mode (wm30, cas,
   menu, mode) a served request, its refusals, the exe capability marks (the CAS arrival state, @mode:, the `page` line), the JSON line; the game-side
   strings the lowering names, pinned as text.
6. S110 SCENEMOMENTS (docs\research\s110\SCENEMOMENTS.md): each bus moment's lowered body pinned line for line, its `# expect:` and `# recipe:`;
   several CAS pages in one request (the `cvar` shape), the refusals by class (a template chooser, a non-leavable page in the middle), the settle
   (a band-aid for the preview model's load, run 273); S110 SCENEMOM2: every match moment but the replay's ends at the finish (`until finish`),
   the table break and finish=table.
`--scene PATH` loads another module with the same names (lower, write, Refused): how a stub is shown to fail the planted cases.
"""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
import re
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
SCENES = os.path.join(ROOT, 'tools', 'input_scripts', 'scenes')
sys.path.insert(0, os.path.join(ROOT, 'tools', 'bench'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'run'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'board'))
import route_lint  # noqa: E402

FAILS, PASSES = [], []


def load_scene(path):
    spec = importlib.util.spec_from_file_location('scene_under_test', path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def check(name, ok, detail=''):
    (PASSES if ok else FAILS).append(name)
    print('%s  %s%s' % ('ok  ' if ok else 'FAIL', name, '' if ok or not detail else '\n      ' + detail.replace('\n', '\n      ')))


def semantic(text):
    out = []
    for ln in text.splitlines():
        ln = ln.split('#')[0].strip() if not ln.strip().startswith('#') else ''
        if not ln:
            continue
        cmd = ln.split()[0]
        if cmd in ('log', 'mark', 'include', 'on_fail'):
            continue
        if ln.startswith('cvar wwe2k14_match_go'):
            ln = 'cvar wwe2k14_match_go'
        out.append(ln)
    while out and (out[-1] == 'quit' or out[-1].startswith('wait_frames')):
        out.pop()
    return out


def launch_flags(text):
    """EVERY header flag (SCENE review: comparing only the launch keys hid that the fandango golden adds the owner renderer, which S108 run 1 ran without:
    run_list then adds --wwe2k14_s101_promotions=false, another renderer). A golden's stated flag differences are listed in FLAG_DIFF."""
    d = {}
    for ln in text.splitlines()[:40]:
        m = re.match(r'^#\s*harness:\s*flags\s+(.+)$', ln.strip())
        if m:
            for t in m.group(1).split():
                k, _, v = t[2:].partition('=')
                d[k] = v
    return d


# difference 3 of the fandango golden (stated): a generated route always runs the owner renderer with the event bus and muted audio (scene.py BASE_FLAGS);
# S108 run 1 ran without them (and so, through run_list's hidden addition, with the S101 promotions off).
FLAG_DIFF = {'s108_fandango_entrance.txt': {'wwe2k14_rdr_owner': 'true', 'wwe2k14_events': 'true', 'audio_mute': 'true'}}


def golden_text(name):
    with open(os.path.join(SCENES, name), encoding='utf-8') as fh:
        return fh.read()


def fandango_expected(gold):
    sem = semantic(gold)
    sem = [s for s in sem if s != 'wait_file BG04.PAC 900']   # difference 1: the arena's own file is the launcher's captured arena, not a request field
    a = sem.index('screenshot sc_fan_00')
    b = sem.index('screenshot sc_fan_13')
    # difference 2: 14 hand-copied shots (at +0 .. +520) = the route's `screenshot_every`, 14 shots at +40 .. +560. The wait is N x K + 1: the runner stops the
    # periodic shot in the same frame the wait ends, before that frame's shot (src\input_script.h OnFrame :516-537, wait_frames :1714), so N x K gave K - 1 shots.
    return sem[:a] + ['screenshot_every 40 sc_fan dup', 'wait_frames 561', 'screenshot_every 0'] + sem[b + 1:]


def golden_requests():
    # golden: a0..a3 at +240/+245/+250/+255, b0..b3 at +655 .. +670, c0 at +1070, c1 at +1370
    hair = ['scene #210/0 vs #103/2; player=0; entrances=off', 'at roster: passive 1 on']
    for name, off in (('a0', 240), ('a1', 245), ('a2', 250), ('a3', 255), ('b0', 655), ('b1', 660), ('b2', 665), ('b3', 670), ('c0', 1070), ('c1', 1370)):
        hair.append('at roster+%df: screenshot hair_%s' % (off, name))
    band = ['scene Undertaker vs Lesnar; arena=53; entrances=off', 'at roster: passive 1 on', 'at roster+300f: shot stage 114 band']
    ban = ['scene Undertaker vs Lesnar; arena=53; entrances=off', 'at roster: passive 1 on']
    for off, st, sh in ((300, 'log 0x8392DD8C; log 0x836F3940', 'ban53_00'), (700, 'log 0x8392DD8C; log 0x836F3940', 'ban53_01'), (1100, 'log 0x836F3940', 'ban53_02'),
                        (1500, 'log 0x8392DD8C; log 0x836F3940', 'ban53_03'), (1900, '', 'ban53_04'), (2300, '', 'ban53_05')):
        ban.append('at roster+%df: %sscreenshot %s' % (off, st + '; ' if st else '', sh))
    caa = ['scene ; arena=200; entrances=off', 'at bell: screenshot caa_m_00', 'at roster: passive 1 on']
    for i, off in enumerate((120, 420, 720, 1020, 1320), 1):
        caa.append('at roster+%df: screenshot caa_m_%02d' % (off, i))
    bld = ['scene Cena vs Lesnar; arena=5; blood=on', 'at bell+120f: passive 1 on', 'at bell+420f: momentum 0 100; stock 0 1; bleed 0 1', 'at bell+540f: bleed 0 2',
           'at bell+660f: bleed 0 3', 'at bell+1060f: shot face 0 face_level3', 'at finish:']
    for off in (1900, 2000, 2300, 2700, 3200, 3700):
        bld.append('at finish+%df: shot face 0 after_%d' % (off, off))
    fan = ['scene Fandango vs Daniel Bryan; dlc=on', 'at entrance:0:1: every 40f x14 sc_fan']
    return [('s108_bld7_bloodon.txt', bld, None), ('s108_shot_band.txt', band, None), ('s108_hair_match.txt', hair, None),
            ('s108_fandango_entrance.txt', fan, fandango_expected), ('s108_caa_match.txt', caa, None), ('s108_ban_arena53.txt', ban, None)]


# S110 SCENEMODES: today's match lowerings, byte-identical: the sha1 of the whole route text scene.py printed for each request BEFORE the modes landed
# (main 542ca46dc). A change to the match lowering that is meant re-pins here, with the reason in its commit.
PINS = [
    ('2bc3bbf59eac644b0de9abb62c5afbb384f4412a', 'scene Cena vs Lesnar; arena=5; blood=on // at bell+120f: passive 1 on // at bell+420f: momentum 0 100; stock 0 1; bleed 0 1 // at finish: // at finish+1900f: shot face 0 after_1900'),
    ('3409dcda58463d61d4265d3066fb49e1e9e7094b', 'scene Undertaker vs Lesnar; arena=53; entrances=off // at roster: passive 1 on // at roster+300f: shot stage 114 band'),
    ('6329018e216621af595e42acb9e4bdca7116a515', 'scene #210/0 vs #103/2; player=0; entrances=off // at roster: passive 1 on // at roster+240f: screenshot hair_a0'),
    ('0addd33844589948e4af39509fa26c6a2eda4b83', 'scene Fandango vs Daniel Bryan; dlc=on // at entrance:0:1: every 40f x14 sc_fan'),
    ('bc38cb02c297efd0727c2e2de467011443ac765e', 'scene ; arena=200; entrances=off // at bell: screenshot caa_m_00 // at roster: passive 1 on'),
    ('981329329c7203211b07996f150774eba4c23919', 'scene Cena vs Lesnar; blood=on // at bell+120f: passive 1 on // at finish: // at victory+600f: shot face 0 v1'),
    ('5a22963fb50fa83b8718aabedf3b9db96220644e', 'scene Cena vs Lesnar; entrances=off; arena=5 // at clock:600: record r1 // at clock:900: every 30f x4 ev'),
    ('b31071a53e73f364cb618149bf0695d11117c361', 'scene Cena vs Lesnar; type=one on one > normal; entrances=off // at roster: log match_clock'),
    ('9350c44dd0002fe186d866e97c71c48c2d90ac7d', 'scene #385 vs Daniel Bryan/3; dlc=on; arena=53; type=one on one > normal; pace=quick; entrances=off; player=1 // at bell+10f: screenshot z'),
    ('5ca452881965e6a4a1b8f25b5648796350d543b5', 'scene Cena/2 vs Lesnar; arena=5; blood=on; entrances=off // at bell+120f: passive 1 on // at finish: // at victory+600f: shot face 0 v1'),
    ('69ab0685fea45456e533f3a4fc91e9aa91129516', 'scene Cena vs Lesnar'),
]


def in_order(steps, want):
    """None when every line of `want` is in `steps` in that order, else the first one missing."""
    i = 0
    for w in want:
        while i < len(steps) and steps[i] != w:
            i += 1
        if i == len(steps):
            return w
        i += 1
    return None


def modes_section(S, tmp, generated):
    """S110 SCENEMODES: per mode a served request (its lines in order, its flags, the trailer), its refusals, the exe capability marks, the JSON line,
    the game-side strings the lowering names. No game; the flow tables are set here (RECORD_TABLES), never read from out\\."""
    tables = getattr(S, 'RECORD_TABLES', None)
    if tables is None:
        check('modes: scene.py has RECORD_TABLES (the S110 SCENEMODES lowering)', False, 'absent')
        return
    tables['cas'], tables['menu'] = None, None   # no flow table: a record is not checked offline (the game refuses an unknown one itself)
    card = list(getattr(S, 'WM30_CARD', ['(absent)']))
    served = [   # (name, request, lines the route holds in this order, flags it holds)
        ('wm30 18: the direct start, the READY card, its A, the bell', 'scene wm30 18 // at bell+60f: screenshot b',
         card + [getattr(S, 'WM30_READY', '(absent)'), 'wait_state match_clock >0 30000', 'wait_frames 60', 'screenshot b'], ['--wwe2k14_wm30=18', '--wwe2k14_wm30_direct']),
        ('wm30 0 at the card: no A', 'scene wm30 0 // at card: screenshot c', card + ['screenshot c', 'log bench: done'], ['--wwe2k14_wm30=0', '--wwe2k14_wm30_direct']),
        ('cas new at record 76, no exe: the go-to\'s own bound', 'scene cas new // at page=76 : screenshot tops',
         ['wait_state seq 76 3600 2', 'wait_frames 1800', 'screenshot tops'], ['--wwe2k14_launch=@cas', '--wwe2k14_cas_goto=76']),
        ('cas new: the editor only', 'scene cas new', ['wait_state seq 76 3600 2', 'log bench: done'], ['--wwe2k14_launch=@cas']),
        ('menu: two records, home between', 'scene menu // at page=1501 : screenshot a // at 1502+30f: screenshot b',
         ['wait_state menu 1 1200 30', 'page 1501', 'screenshot a', 'home', 'page 1502', 'wait_frames 30', 'screenshot b'], []),
        ('menu: no record = the main menu', 'scene menu', ['wait_state menu 1 1200 30', 'log bench: done'], []),
        ('mode threads', 'scene mode threads // at arrived: screenshot t', ['wait_state seq 77 3600 2', 'screenshot t'], ['--wwe2k14_launch=@mode:threads']),
        ('mods=on on a match', 'scene Cena vs Lesnar; mods=on', ['wait_state match_clock >0 30000'], ['--wwe2k14_mods_superstars=true']),
        ('mods=on, dlc=on on cas', 'scene cas new; mods=on; dlc=on', ['wait_state seq 76 3600 2'], ['--wwe2k14_mods_superstars=true', '--wwe2k14_dlc=true']),
        ('`scene match WHO` = `scene WHO`', 'scene match Cena vs Lesnar', ['wait_state match_clock >0 30000'], ['--wwe2k14_launch_participants=139,343']),
    ]
    for name, req, lines, flags in served:
        try:
            text, fl, stem = S.lower(req)
        except S.Refused as x:
            check('served: ' + name, False, 'refused: %s' % x)
            continue
        steps = [ln for ln in text.splitlines() if ln and not ln.startswith('#')]
        miss = in_order(steps, lines)
        check('served: ' + name, miss is None and all(f in fl.split() for f in flags), 'missing (in order): %r; flags %s' % (miss, fl))
        check('trailer (on_fail quit, boot, bench: done, quit): ' + name, steps[0] == 'on_fail quit' and 'include boot_to_menu' in steps
              and steps[-3:] == ['log bench: done', 'wait_frames 120', 'quit'], str(steps[:1] + steps[-3:]))
        generated.append(S.write(req, out_dir=tmp))
    # every row of src\mode_table.h but cas (that one is `cas new`) lowers, waits on its own sequence and files on the board (the one source of names)
    try:
        check('the mode table parser\'s own selftest (tools\\scene\\mode_table.py --selftest: the header\'s shape)', S.mode_table().selftest() == 0)
        for row in [r for r in S.mode_table().rows(ROOT) if r['name'] != 'cas']:
            try:
                text = S.lower('scene mode ' + row['name'])[0]
                ok = ('--wwe2k14_launch=@mode:%s' % row['name']) in text and ('\nwait_state seq %d 3600 2\n' % row['seq']) in text
                check('served: every table row: mode %s' % row['name'], ok, text[:300])
            except S.Refused as x:
                check('served: every table row: mode %s' % row['name'], False, str(x))
    except AttributeError as x:
        check('served: every table row (scene.py reads src\\mode_table.h)', False, str(x))
    # refusals, a few per mode: each exits 2 with its reason (the flow tables set: records are checked)
    tables['cas'], tables['menu'] = {76: None, 3: 1003}, {1501: 1500, 1502: 2}
    refused = [
        ('wm30: N out of range', 'scene wm30 46', '0-45'),
        ('wm30: N not a number', 'scene wm30 eighteen', '0-45'),
        ('wm30: the card after a match moment', 'scene wm30 3 // at bell: screenshot a // at card: screenshot b', 'before every match moment'),
        ('wm30: a match field', 'scene wm30 3; arena=5', 'match field'),
        ('cas: an unknown record (the flow table)', 'scene cas new // at page=9999 : screenshot a', 'not in the editor'),
        ('cas: a template chooser record (S110 SCENEMOMENTS: two records are served now)', 'scene cas new // at 76: screenshot a // at 3: screenshot b', 'template chooser'),
        ('cas: not a new superstar', 'scene cas seat', 'NEW superstar'),
        ('cas: a match item', 'scene cas new // at 76: passive 1 on', 'match item'),
        ('cas: a match field', 'scene cas new; arena=5', 'match field'),
        ('menu: an unknown record (the flow table)', 'scene menu // at page=4242 : screenshot a', 'not in the front-end flow table'),
        ('menu: a match moment', 'scene menu // at bell: screenshot a', 'no position of menu'),
        ('menu: an offset back on one record', 'scene menu // at 1501+60f: screenshot a // at 1501+10f: screenshot b', 'in order'),
        ('mode: a name not in the table', 'scene mode wrestlemania', 'not a mode of the table'),
        ('mode: a name the old hand list had (moves), not src\\mode_table.h', 'scene mode moves', 'moveset'),
        ('mode: the cas row is `cas new` (one way in)', 'scene mode cas', 'scene cas new'),
        ('mode: a position other than arrived', 'scene mode logo // at page=3: screenshot a', 'no position of mode'),
        ('stage: not offered yet', 'scene stage 12', 'not offered'),
    ]
    for name, req, want in refused:
        try:
            S.lower(req)
            check('refused: ' + name, False, 'the request was served')
        except S.Refused as x:
            check('refused: ' + name, want.lower() in str(x).lower(), 'refused, but the reason lacks %r: %s' % (want, x))
    for name, req, want in (('menu: a record with its menu in the table is waited on', 'scene menu // at 1501: screenshot a', 'wait_state menu 1500 600 2'),
                            ('cas: a record in the table is served', 'scene cas new // at 76: screenshot a', '--wwe2k14_cas_goto=76')):
        try:
            check('served: ' + name, want in S.lower(req)[0], want)
        except S.Refused as x:
            check('served: ' + name, False, str(x))
    # S110 MODEPICK: `scene mode moveset #N` -> the pick flag and its expected line; a pick elsewhere, or out of 0-499, is refused
    for name, req, want in (('mode moveset #N: the pick flag', 'scene mode moveset #397', '--wwe2k14_mode_pick=397'),
                            ('mode moveset #N: its picked line is expected', 'scene mode moveset #397', '# expect: [wwe2k14][launch] @mode:moveset: picked wrestler 397 ('),
                            ('mode entrance #N: the pick flag (S110 ENTPICK)', 'scene mode entrance #397', '--wwe2k14_mode_pick=397'),
                            ('mode entrance #N: its picked line is expected', 'scene mode entrance #397', '# expect: [wwe2k14][launch] @mode:entrance: picked wrestler 397 (')):
        try:
            check('served: ' + name, want in S.lower(req)[0], want)
        except (S.Refused, AttributeError) as x:
            check('served: ' + name, False, str(x))
    for name, req, want in (('a wrestler pick on a row without one', 'scene mode logo #3', 'pick'), ('a pick out of the roster range', 'scene mode moveset #500', '0-499')):
        try:
            S.lower(req)
            check('refused: ' + name, False, 'served')
        except S.Refused as x:
            check('refused: ' + name, want in str(x), str(x))
    tables['cas'], tables['menu'] = None, None
    # the exe decides what it can serve (route_lint.exe_knows over its strings): the CAS arrival state only when the exe holds its name
    exes = {}
    for folder, names in (('cas_old', ['wwe2k14_launch', '@cas', 'wwe2k14_cas_goto']), ('cas_new', ['wwe2k14_launch', '@cas', 'wwe2k14_cas_goto', getattr(S, 'CAS_RECORD_STATE', '(absent)')]),
                          ('cas_none', ['wwe2k14_launch', '@cas']), ('mode_old', ['wwe2k14_launch']), ('mode_new', ['wwe2k14_launch', '@mode:']),
                          ('menu_old', ['wwe2k14_launch']), ('menu_new', [getattr(S, 'MENU_PAGE_MARK', '(absent)')]), ('wm30_old', ['wwe2k14_wm30'])):
        os.makedirs(os.path.join(tmp, 'x_' + folder))   # route_lint.exe_knows caches by exe FOLDER: one folder each
        exes[folder] = os.path.join(tmp, 'x_' + folder, 'game.exe')
        with open(exes[folder], 'wb') as fh:
            fh.write(b'MZ\0' + b'\0'.join(n.encode() for n in names) + b'\0')
    cas_req = 'scene cas new // at page=76 : screenshot tops'
    for name, req, exe, want_in, want_out, kind in (
            ('cas on an exe without the arrival state: the frame count, the log line is the test', cas_req, 'cas_old', 'wait_frames 1800', 'cas_record', 'frames'),
            ('cas on an exe with the arrival state (lane CASARRIVE): cas_record == the record', cas_req, 'cas_new', '\nwait_state cas_record 76 3600\n', 'wait_frames 1800', 'state'),
            ('cas on an exe with the arrival state: its state line is expected too', cas_req, 'cas_new', '# expect: [wwe2k14][cas_goto] state cas_goto=2 cas_record=76 ', '(none)', 'state'),
            ('mode on an exe with @mode:', 'scene mode logo', 'mode_new', 'wait_state seq 78 3600 2', '(none)', 'state'),
            ('menu on an exe with the page line', 'scene menu // at 1501: screenshot a', 'menu_new', 'page 1501', '(none)', 'page')):
        try:
            t = S.lower(req, exe=exes[exe])[0]
            d = S.describe(t)
            check('served: ' + name, want_in in t and want_out not in t and d['arrival']['kind'] == kind, '%s / %s' % (want_in, d['arrival']))
        except (S.Refused, AttributeError) as x:
            check('served: ' + name, False, str(x))
    for name, req, exe, want in (('cas on an exe without the go-to', cas_req, 'cas_none', 'wwe2k14_cas_goto'),
                                 ('mode on an exe without @mode: (lane MODELAUNCH not in it)', 'scene mode logo', 'mode_old', '@mode:'),
                                 ('menu on an exe without the `page` line', 'scene menu // at 1501: screenshot a', 'menu_old', 'page N'),
                                 ('wm30 on an exe without the direct start', 'scene wm30 3', 'wm30_old', 'wwe2k14_wm30_direct')):
        try:
            S.lower(req, exe=exes[exe])
            check('refused: ' + name, False, 'served')
        except S.Refused as x:
            check('refused: ' + name, want in str(x), str(x))
    # the JSON line: header flags, arrival (kind, line, game lines to expect), the verdict line, the shots
    keys = {'route', 'stem', 'request', 'flags', 'intent', 'arrival', 'verdict', 'shots', 'shot_count'}
    for req, kind, line, expect, shots in (
            (cas_req, 'frames', 'wait_frames 1800', ['[wwe2k14][launch] @cas: sequence 76 is current at frame', '[wwe2k14][cas_goto] record 76: arrived at record 76'], 1),
            ('scene wm30 18 // at bell: every 30f x4 ev', 'state', 'wait_state match_clock >0 30000', ['[wwe2k14][wm30][direct] exit action 3 requested'], 4),
            ('scene menu // at 1501: screenshot a', 'page', 'page 1501', ['[wwe2k14][menu_goto] record 1501 page change done'], 1),
            ('scene mode logo', 'state', 'wait_state seq 78 3600 2', ['[wwe2k14][launch] @mode:logo: enter gates pass (', '[wwe2k14][launch] @mode:logo: exit 19 sent at frame',
                                                                       '[wwe2k14][launch] @mode:logo: sequence 78 is current at frame'], 0),
            ('scene Cena vs Lesnar; entrances=off // at bell+10f: screenshot z', 'state', 'wait_state match_clock >0 30000', [], 1)):
        try:
            d = S.describe(S.lower(req)[0])
            ok = set(d) == keys and d['arrival'] == {'kind': kind, 'line': line, 'expect': expect} and d['shot_count'] == shots and d['verdict'] == 'bench: done'
            check('json: ' + req, ok, json.dumps(d))
        except (S.Refused, AttributeError) as x:
            check('json: ' + req, False, str(x))
    for argv, code, key in ((['--print', 'scene wm30 46', '--json'], 2, 'refused'), (['--print', 'scene cas new', '--json'], 0, 'arrival'),
                            (['--print', 'scene mode wrestlemania'], 2, None)):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = S.main(argv)
        out = buf.getvalue().strip().splitlines()
        ok = rc == code and len(out) == 1 and (key in json.loads(out[0]) if key else out[0].startswith('REFUSED: '))
        check('cli %s: exit %d, one line' % (' '.join(argv), code), ok, '%s %r' % (rc, out))
    # the game side the lowering names: each string pinned as text in the source that prints / reads it
    src = lambda p: open(os.path.join(ROOT, *p.split('/')), encoding='utf-8').read()
    caption = src('tools/input_scripts/scenes/s105_chk_caption.txt').splitlines()
    pins = [
        ('the READY card lines are the S105 route\'s, in its order', in_order(caption, card + [getattr(S, 'WM30_READY', '(absent)')]) is None),
        ('wm30 N is 0-45', '(0-45' in src('src/wm30_jump.h') and getattr(S, 'WM30_LAST', None) == 45),
        ('the direct start logs its exit action', '"[wwe2k14][wm30][direct] exit action {} requested' in src('src/wm30_boot.h') and 'exit action 3' in src('src/wm30_boot.h')),
        ('the launch lines the cas / mode routes expect are printed by src/mode_launch.h (tools\\scene\\mode_table.py)', not S.mode_table().log_formats_missing(ROOT)),
        ('the CAS state line, once lane CASARRIVE is in: src/cas_goto.h prints `state cas_goto={} cas_record={} `', 'cas_record' not in src('src/cas_goto_core.h')
         or '"[wwe2k14][cas_goto] state cas_goto={} cas_record={} ' in src('src/cas_goto.h')),
        ('the go-to logs `record R: ... arrived at record R`', '"[wwe2k14][cas_goto] record {}: {} (frame {})"' in src('src/cas_goto.h')
         and src('src/cas_goto.h').count('Stop(fmt::format("arrived at record {}') == 2),
        ('menu_goto logs `record R page change done`', '"[wwe2k14][menu_goto] record {} page change done' in src('src/menu_goto.h')),
        ('the exe string of the `page` line', getattr(S, 'MENU_PAGE_MARK', '(absent)') in src('src/menu_goto_core.h')),
        ('the route runner has `page` and `home`', 'cmd == "page"' in src('src/input_script.h') and 'cmd == "home"' in src('src/input_script.h')),
    ]
    for name, ok in pins:
        check('game side: ' + name, ok)


def moments_section(S, tmp, generated):
    """S110 SCENEMOMENTS: each bus moment's lowered body pinned line for line (the steps between `include boot_to_menu` and `bench: done`), its
    `# expect:` lines (describe()) and a `# recipe:` text of its header; several CAS pages in one request; the CAS refusals by class; the settle."""
    if not hasattr(S, '_moments'):
        check('moments: scene.py lowers the bus moments (tools\\run\\scene_moments.py)', False, 'absent')
        return

    def body(text):
        steps = [ln for ln in text.splitlines() if ln and not ln.startswith('#')]
        return steps[steps.index('include boot_to_menu') + 1:steps.index('log bench: done')]
    L = ['wait_file INGAMELUA.PAC 1800', 'log bench: match launched', 'wait_state match_clock >0 30000']
    R0 = ['momentum 0 100', 'stock 0 1', 'passive 1 on']
    base = 'scene Cena vs Lesnar; entrances=off // '
    table = 'scene #324 vs #343; type=68; entrances=off // '   # S110 SCENEMOM2: a one-on-one TABLE match (db\ids\match_types.json id 68)
    pin = '"kind": "match", "what": "pin"'
    cases = [   # (name, request, the body, the expect lines, a text of the header); S110 SCENEMOM2: every wait but the replay's ends at the finish
        ('pin', base + 'at pin : screenshot p', L + R0 + ['wait_event pin 72000 until finish', 'screenshot p'], [pin],
         '# recipe: momentum 0 100, stock 0 1, passive 1 on, for `wait_event pin 72000 until finish`'),
        ('count=2', base + 'at count=2 : screenshot c', L + R0 + ['wait_event count count=2 72000 until finish', 'screenshot c'],
         ['"kind": "match", "what": "count", "count": 2'], '# recipe: momentum 0 100'),
        ('kickout (no recipe)', base + 'at kickout : screenshot k', L + ['wait_event kickout 72000 until finish', 'screenshot k'], ['"kind": "match", "what": "kickout"'],
         '# recipe: none for `wait_event kickout 72000 until finish`'),
        ('kickout=2', base + 'at kickout=2 : screenshot k', L + ['wait_event kickout at_count=2 72000 until finish', 'screenshot k'],
         ['"kind": "match", "what": "kickout", "at_count": 2'], '# recipe: none'),
        ('finish=pinfall', base + 'at finish=pinfall : screenshot f', L + R0 + ['wait_event finish finish_name=pinfall 72000 until finish', 'screenshot f'],
         ['"what": "finish", "finish_type": 1, "finish_name": "pinfall", "winner_slot": '], '# recipe: momentum 0 100'),
        ('finish=submission (no recipe)', base + 'at finish=submission : screenshot f',
         L + ['wait_event finish finish_name=submission 72000 until finish', 'screenshot f'], ['"what": "finish", "finish_type": 6, "finish_name": "submission", "winner_slot": '],
         'ends the match by pinfall'),
        ('finish=dq: the bus spelling', base + 'at finish=dq : screenshot f', L + ['wait_event finish finish_name=disqualification 72000 until finish', 'screenshot f'],
         ['"what": "finish", "finish_type": 3, "finish_name": "disqualification", "winner_slot": '], '# recipe: none'),
        ('move by its fields, seat 1', base + 'at move:class=finisher:slot=1 : screenshot m',
         L + ['momentum 1 100', 'stock 1 1', 'passive 0 on', 'wait_event move class=finisher slot=1 72000 until finish', 'screenshot m'], ['"kind": "move", "slot": 1'],
         '# recipe: momentum 1 100, stock 1 1, passive 0 on'),
        ('eliminated with type= (a fatal 4-way battle royal)', 'scene #139 vs #343 vs #324 vs #161; type=34; entrances=off // at eliminated : screenshot e',
         L + ['wait_event eliminated 72000 until finish', 'screenshot e'], ['"kind": "match", "what": "eliminated"'], '# recipe: none'),
        ('replay (after the finish: no until)', base + 'at replay : screenshot r', L + R0 + ['wait_event replay start 72000', 'screenshot r'],
         ['"kind": "replay", "state": "start"'], '# recipe: momentum 0 100'),
        ('finish=pinfall then replay_end: one recipe', base + 'at finish=pinfall : screenshot f // at replay_end : screenshot r',
         L + R0 + ['wait_event finish finish_name=pinfall 72000 until finish', 'screenshot f', 'wait_event replay end 72000', 'screenshot r'],
         ['"what": "finish", "finish_type": 1, "finish_name": "pinfall", "winner_slot": ', '"kind": "replay", "state": "end"'], '# recipe: momentum 0 100'),
        ('ring_break after the request\'s own lines', base + 'at bell : momentum 0 100 // at ring_break : screenshot b',
         L + ['momentum 0 100', 'wait_event break ring 72000 until finish', 'screenshot b'], ['"kind": "break", "what": "ring"'],
         '# recipe: the request\'s own director lines'),
        ('the request\'s own verbs: no default recipe', base + 'at bell+120f : passive 1 on // at pin : screenshot p',
         L + ['wait_frames 120', 'passive 1 on', 'wait_event pin 72000 until finish', 'screenshot p'], [pin], '# recipe: the request\'s own'),
        # review: a director verb before the bell answers busy and is retried for only 1800 frames (src\situation_director.h), so the
        # recipe waits for the bell first, never placed during the entrances
        ('an entrance first: the bell wait, then the recipe', 'scene Cena vs Lesnar // at entrance:0:1 : screenshot e // at pin : screenshot p',
         ['wait_file INGAMELUA.PAC 1800', 'log bench: match launched', 'wait_state entrance_step_0 >=1 3600 2', 'screenshot e', 'wait_state match_clock >0 30000']
         + R0 + ['wait_event pin 72000 until finish', 'screenshot p'], [pin], '# recipe: momentum 0 100'),
        # review: a Rumble's eliminated line prints "n" before "slot" (src\event_bus_rumble.h): the slot is the wait's filter, not the expect's text
        ('eliminated by slot: the expect is the head only', 'scene #139 vs #343 vs #324 vs #161; type=34; entrances=off // at eliminated:slot=1 : screenshot e',
         L + ['wait_event eliminated slot=1 72000 until finish', 'screenshot e'], ['"kind": "match", "what": "eliminated"'], '# recipe: none'),
        ('the bell then a pin: the recipe right after the bell wait', base + 'at bell+300f : screenshot b // at pin : screenshot p',
         L + R0 + ['wait_frames 300', 'screenshot b', 'wait_event pin 72000 until finish', 'screenshot p'], [pin], '# recipe: momentum 0 100'),
        ('a pin named twice is one wait', base + 'at pin : screenshot a // at pin+30f : screenshot b',
         L + R0 + ['wait_event pin 72000 until finish', 'screenshot a', 'wait_frames 30', 'screenshot b'], [pin], '# recipe: momentum 0 100'),
        # S110 SCENEMOM2: the table break (src\table_break.h's bus event) in a Tables match, no recipe (the match's own course); the finish name `table`
        ('table_break in a TABLE match (no recipe)', table + 'at table_break : screenshot b', L + ['wait_event break table 72000 until finish', 'screenshot b'],
         ['"kind": "break", "what": "table"'], '# recipe: none for `wait_event break table 72000 until finish`: no recipe: the Tables match'),
        ('finish=table', table + 'at finish=table : screenshot f', L + ['wait_event finish finish_name=table 72000 until finish', 'screenshot f'],
         ['"what": "finish", "finish_type": 2, "finish_name": "table", "winner_slot": '], '# recipe: none'),
        # S110 FIRETABLE: a table set on fire by the seat's wish, broken, the replay skipped by the game's own skip (its bus line waited right after)
        ('fire, table_break, skip the replay', table + 'at bell : momentum 0 100; fire 0 // at table_break : screenshot b // at replay+60f : screenshot a; '
         'skip_replay; screenshot c // at replay_end+30f : screenshot e',
         L + ['momentum 0 100', 'fire 0', 'wait_event break table 72000 until finish', 'screenshot b', 'wait_event replay start 72000', 'wait_frames 60',
              'screenshot a', 'skip_replay', 'wait_event replay skipped 600', 'screenshot c', 'wait_event replay end 72000', 'wait_frames 30', 'screenshot e'],
         ['"kind": "break", "what": "table"', '"kind": "replay", "state": "start"', '"kind": "replay", "state": "end"'],
         '# recipe: the request\'s own director lines'),
    ]
    for name, req, want, expect, head in cases:
        try:
            t = S.lower(req)[0]
            d = S.describe(t)
            got = body(t)
            check('moment: ' + name, got == want and d['arrival']['expect'] == expect and head in t,
                  'body %r\nwant %r\nexpect %r\nheader has %r: %s' % (got, want, d['arrival']['expect'], head, head in t))
            generated.append(S.write(req, out_dir=tmp))
        except S.Refused as x:
            check('moment: ' + name, False, 'refused: %s' % x)
    # S110 FIRETABLE review: `fire S` / `skip_replay` on an exe built before the two verbs are refused when the route is lowered (that exe's script
    # load would refuse the line); an exe with them (its log text [wwe2k14][table_fire]) lowers; a request without them is never refused for this
    fire_probe = '[wwe2k14][table_fire]'
    fstub = open(os.path.join(os.path.dirname(os.path.abspath(S.__file__)), 'scene.py'), encoding='utf-8').read().replace(fire_probe, '')
    for folder, extra in (('m_fire_new', fire_probe), ('m_fire_old', '')):
        os.makedirs(os.path.join(tmp, folder))
        with open(os.path.join(tmp, folder, 'game.exe'), 'w', encoding='utf-8') as fh:
            fh.write('MZ\n' + fstub + '\n' + extra + '\n')
    fire_new, fire_old = os.path.join(tmp, 'm_fire_new', 'game.exe'), os.path.join(tmp, 'm_fire_old', 'game.exe')
    for name, req, exe, refused in (
            ('fire on an exe with the verbs lowers', table + 'at bell : momentum 0 100; fire 0 // at table_break : screenshot b', fire_new, False),
            ('skip_replay on an exe with the verbs lowers', table + 'at replay+60f : skip_replay', fire_new, False),
            ('fire on an exe without the verbs is refused', table + 'at bell : momentum 0 100; fire 0 // at table_break : screenshot b', fire_old, True),
            ('skip_replay on an exe without the verbs is refused', table + 'at replay+60f : skip_replay', fire_old, True),
            ('no fire item: an exe without the verbs is not refused for it', table + 'at table_break : screenshot b', fire_old, False)):
        try:
            S.lower(req, exe=exe)
            check('firetable exe: ' + name, not refused, 'lowered, want refused')
        except S.Refused as x:
            check('firetable exe: ' + name, refused and ('fire' in str(x) or 'skip_replay' in str(x)), 'refused: %s' % x)
    # S110 FINPICK: an exe that holds the finisher_fit_S states gets the seat's finisher wish in the finish recipe, then the fit waited for and asserted
    # (valid from the verb's ok: src\finisher_pick_core.h FitState); an exe without them keeps the old recipe (and no submission recipe)
    stub = open(os.path.join(os.path.dirname(os.path.abspath(S.__file__)), 'scene.py'), encoding='utf-8').read().replace(S._moments.FIT_PROBE, '')
    for folder, extra in (('m_fin_new', S._moments.FIT_PROBE), ('m_fin_old', '')):
        os.makedirs(os.path.join(tmp, folder))
        with open(os.path.join(tmp, folder, 'game.exe'), 'w', encoding='utf-8') as fh:
            fh.write('MZ\n' + stub + '\n' + extra + '\n')
    fin_new, fin_old = os.path.join(tmp, 'm_fin_new', 'game.exe'), os.path.join(tmp, 'm_fin_old', 'game.exe')
    W0 = ['finisher 0 nohold', 'wait_state finisher_fit_0 >=0 600 1', 'assert_state finisher_fit_0 1']
    H0 = ['finisher 0 hold', 'wait_state finisher_fit_0 >=0 600 1', 'assert_state finisher_fit_0 1']
    for name, req, exe, want in (
            ('pin: seat 0 wants a finisher that is not a hold', base + 'at pin : screenshot p', fin_new, L + R0 + W0 + ['wait_event pin 72000 until finish', 'screenshot p']),
            ('count=2: nohold', base + 'at count=2 : screenshot c', fin_new, L + R0 + W0 + ['wait_event count count=2 72000 until finish', 'screenshot c']),
            ('finish=pinfall: nohold', base + 'at finish=pinfall : screenshot f', fin_new,
             L + R0 + W0 + ['wait_event finish finish_name=pinfall 72000 until finish', 'screenshot f']),
            ('finish=submission: the finish recipe with a hold wish', base + 'at finish=submission : screenshot f', fin_new,
             L + R0 + H0 + ['wait_event finish finish_name=submission 72000 until finish', 'screenshot f']),
            ('replay: no wish (any finish ends the match)', base + 'at replay : screenshot r', fin_new, L + R0 + ['wait_event replay start 72000', 'screenshot r']),
            ('an exe without the states: the old pin recipe', base + 'at pin : screenshot p', fin_old, L + R0 + ['wait_event pin 72000 until finish', 'screenshot p']),
            ('an exe without the states: no submission recipe', base + 'at finish=submission : screenshot f', fin_old,
             L + ['wait_event finish finish_name=submission 72000 until finish', 'screenshot f'])):
        try:
            t = S.lower(req, exe=exe)[0]
            check('finpick: ' + name, body(t) == want, 'body %r\nwant %r' % (body(t), want))
        except S.Refused as x:
            check('finpick: ' + name, False, 'refused: %s' % x)
    # S110 OMGVERB: a dev exe with the `omg` verb gets the ring-break recipe (seat 0 named in WHO); an exe without the verb keeps the refusal;
    # a request with no WHO is refused (the recipe needs seat 0 to be a super heavyweight)
    os.makedirs(os.path.join(tmp, 'm_omg_new'))
    with open(os.path.join(tmp, 'm_omg_new', 'game.exe'), 'w', encoding='utf-8') as fh:
        fh.write('MZ\n' + stub.replace(S._moments.OMG_PROBE, '') + '\n' + S._moments.OMG_PROBE + '\n')
    omg_new = os.path.join(tmp, 'm_omg_new', 'game.exe')
    RING = ['wait_state 0x836F38D0>2812 <1126170624 3600 2', 'passive 1 on', 'stock 0 3', 'omg 0 ring', 'wait_state omg_fit_0 >=0 600 1',
            'assert_state omg_fit_0 1', 'state 1 CornerGroggy', 'wait_state omg_fit_0 >=3 3600 1', 'assert_state omg_fit_0 3']
    big = 'scene #125 vs #139; entrances=off // '
    try:
        t = S.lower(big + 'at ring_break : screenshot b', exe=omg_new)[0]
        check('omgverb: the ring-break recipe on a dev exe with the verb', body(t) == L + RING + ['wait_event break ring 72000 until finish', 'screenshot b'],
              'body %r' % body(t))
        check('omgverb: the expect line', '"kind": "break", "what": "ring"' in t, t[:600])
        check('omgverb: no momentum line (a full meter refuses the ring break)', not any(s.startswith('momentum') for s in body(t)), body(t))
    except S.Refused as x:
        check('omgverb: the ring-break recipe on a dev exe with the verb', False, 'refused: %s' % x)
    for name, req, exe, want in (
            ('an exe without the verb keeps the refusal', big + 'at ring_break : screenshot b', fin_old, 'no recipe reaches'),
            ('no WHO: seat 0 must be named', 'scene ; entrances=off // at ring_break : screenshot b', omg_new, 'super heavyweight'),
            ('the request\'s own lines only after it', big + 'at ring_break : screenshot b // at finish : momentum 0 50', omg_new, 'earlier position')):
        try:
            S.lower(req, exe=exe)
            check('omgverb: ' + name, False, 'not refused')
        except S.Refused as x:
            check('omgverb: ' + name, want in str(x), str(x))
    # S110 TOOLFIT: the finish expect is the finish line's text from "what" to winner_slot: a multi-fall rule's `fall` line (S110 ELIMFIN,
    # src\event_bus.h :421: the same record after "what": "fall") must not hold it (run 299 cited the first fall); the type per name is FinishName's
    M = S._moments
    fx = M.expect(('finish', (('finish_name', 'pinfall'),)))
    rec = '"finish_type": 1, "finish_name": "pinfall", "winner_slot": 0, "winner_id": 139, "loser_slot": 1'
    fall_line = '[wwe2k14][event] {"f": 22741, "t": 379.0, "kind": "match", "what": "fall", "scorer_slot": 0, "scorer_id": 139, "falls": 1, ' + rec
    fin_line = '[wwe2k14][event] {"f": 72217, "t": 1203.6, "kind": "match", "what": "finish", ' + rec + ', "winner_falls": 3, "loser_falls": 1}'
    check('toolfit: the finish expect holds in the finish line, never in a fall line', fx in fin_line and fx not in fall_line, fx)
    bus = open(os.path.join(ROOT, 'src', 'event_bus.h'), encoding='utf-8').read()
    fn = bus[bus.index('inline const char* FinishName('):]
    fn = fn[:fn.index('\n}\n')]
    types = {nm: int(t) for t, nm in re.findall(r'case (\d+): return "([a-z ]+)";', fn)}
    k1 = open(os.path.join(ROOT, 'src', 'table_break_core.h'), encoding='utf-8').read()
    if 'case 2: return table_break::Kind1Name(' in fn and '? "table" : "count-out"' in k1:
        types.update({'table': 2, 'count-out': 2})
    check('toolfit: FINISH_TYPES is src\\event_bus.h FinishName (and Kind1Name for type 2)', types == M.FINISH_TYPES and set(types) == set(M.FINISH_NAMES.values()),
          'FinishName %r\nFINISH_TYPES %r' % (types, M.FINISH_TYPES))
    try:   # a 30 Years match: the bus moment after the READY card's A, its line expected beside the card's
        t = S.lower('scene wm30 18 // at pin : screenshot p')[0]
        d = S.describe(t)
        check('moment: wm30 18 at pin', in_order(body(t), [S.WM30_READY, 'wait_state match_clock >0 30000'] + R0 + ['wait_event pin 72000 until finish', 'screenshot p']) is None
              and d['arrival']['expect'] == [S.WM30_EXPECT, pin] and '# recipe: momentum 0 100' in t, str(body(t)) + str(d['arrival']))
    except S.Refused as x:
        check('moment: wm30 18 at pin', False, str(x))
    # several Create a Superstar pages: an exe that holds the go-to's route states (lane CASARRIVE: cas_record, cas_target)
    tables = S.RECORD_TABLES
    tables['cas'] = None
    exes = {}
    for folder, names in (('m_cas_new', ['wwe2k14_launch', '@cas', 'wwe2k14_cas_goto', 'cas_record', 'cas_target']),
                          ('m_cas_old', ['wwe2k14_launch', '@cas', 'wwe2k14_cas_goto', 'cas_record'])):
        os.makedirs(os.path.join(tmp, folder))
        exes[folder] = os.path.join(tmp, folder, 'game.exe')
        with open(exes[folder], 'wb') as fh:
            fh.write(b'MZ\0' + b'\0'.join(n.encode() for n in names) + b'\0')

    def page(r, shot):
        return ['cvar wwe2k14_cas_goto %d' % r, 'wait_state cas_target %d 60 1' % r, 'wait_state cas_goto >=2 3000', 'log_state cas_reason',
                'assert_state cas_goto 2', 'assert_state cas_record %d' % r, 'screenshot ' + shot]
    three = 'scene cas new // at page=76 : screenshot tops // at page=36 : screenshot mustache // at page=0 : screenshot head'
    try:
        t, fl, stem = S.lower(three, exe=exes['m_cas_new'])
        d = S.describe(t)
        want_e = [S.mode_table().expect_lines(S.mode_row('cas'))[2]] + [e for r in (76, 36, 0) for e in (S.CAS_EXPECT_ARRIVED % (r, r), S.CAS_EXPECT_STATE % r)]
        # S110 CASREADY: no settle (the go-to's arrival waits for the preview: run 278, sweep 3)
        check('cas: three pages across two tabs: each by cvar, the cas_goto_core.h shape, no settle before a shot',
              body(t) == ['wait_state seq 76 3600 2'] + page(76, 'tops') + page(36, 'mustache') + page(0, 'head') and '--wwe2k14_cas_goto' not in fl
              and d['arrival'] == {'kind': 'state', 'line': 'assert_state cas_record 0', 'expect': want_e} and stem.startswith('create_cas_76_36_0_')
              and '# settle' not in t,
              '%r\n%s\n%s' % (body(t), fl, d['arrival']))
        generated.append(S.write(three, exe=exes['m_cas_new'], out_dir=tmp))
    except S.Refused as x:
        check('cas: three pages across two tabs', False, str(x))
    try:
        t = S.lower('scene cas new // at page=76 : screenshot a // at page=43 : screenshot b', exe=exes['m_cas_new'])[0]
        check('cas: a LETTERING page as the last page is served', 'assert_state cas_record 43' in t, t[-400:])
    except S.Refused as x:
        check('cas: a LETTERING page as the last page is served', False, str(x))
    try:   # one page: the boot flag and the cas_record wait as before, plus the settle; an offset past it adds no wait
        t1 = S.lower('scene cas new // at page=76 : screenshot tops', exe=exes['m_cas_new'])[0]
        t2 = S.lower('scene cas new // at page=76+200f : screenshot tops', exe=exes['m_cas_new'])[0]
        check('cas: one page keeps its flag and state wait, no settle before the shot',
              body(t1) == ['wait_state seq 76 3600 2', 'wait_state cas_record 76 3600', 'log_state cas_goto', 'screenshot tops']
              and body(t2)[-2:] == ['wait_frames 200', 'screenshot tops'], '%r / %r' % (body(t1), body(t2)))
    except S.Refused as x:
        check('cas: one page keeps its flag and state wait', False, str(x))
    for name, req, exe, want in (
            ('a LETTERING page in the middle (never left)', 'scene cas new // at page=43 : screenshot a // at page=76 : screenshot b', 'm_cas_new', 'LAST page'),
            ('a CUSTOM LOGO DATA page in the middle', 'scene cas new // at page=180 : screenshot a // at page=0 : screenshot b', 'm_cas_new', 'CUSTOM LOGO DATA'),
            ('a template chooser alone', 'scene cas new // at page=23 : screenshot a', 'm_cas_new', 'template chooser'),
            ('several pages on an exe without cas_target', 'scene cas new // at page=76 : screenshot a // at page=36 : screenshot b', 'm_cas_old', 'cas_target')):
        try:
            S.lower(req, exe=exes[exe])
            check('refused: cas: ' + name, False, 'served')
        except S.Refused as x:
            check('refused: cas: ' + name, want in str(x), str(x))


def main(argv):
    path = os.path.join(HERE, 'scene.py')
    if '--scene' in argv:
        path = os.path.abspath(argv[argv.index('--scene') + 1])
    S = load_scene(path)
    tmp = tempfile.mkdtemp(prefix='scene_selftest_')
    try:
        generated = []

        def req_text(lines):
            return '\n'.join(lines)

        # ---- 1. goldens
        for gold_name, lines, transform in golden_requests():
            gold = golden_text(gold_name)
            try:
                text, flags, stem = S.lower(req_text(lines))
            except S.Refused as x:
                check('golden ' + gold_name, False, 'refused: %s' % x)
                continue
            exp = transform(gold) if transform else semantic(gold)
            got = semantic(text)
            want_flags = dict(launch_flags(gold), **FLAG_DIFF.get(gold_name, {}))
            if got == exp and launch_flags(text) == want_flags:
                check('golden %s: %d steps equal, launch flags equal %s' % (gold_name, len(exp), launch_flags(gold) and sorted(launch_flags(gold))[:2]), True)
            else:
                d = next((i for i, (a, b) in enumerate(zip(got, exp)) if a != b), min(len(got), len(exp)))
                check('golden ' + gold_name, False, 'step %d: got %r, want %r (got %d steps, want %d); flags got %s want %s' % (
                    d, got[d] if d < len(got) else None, exp[d] if d < len(exp) else None, len(got), len(exp), launch_flags(text), want_flags))
            try:
                generated.append(S.write(req_text(lines), out_dir=tmp))
            except S.Refused as x:
                check('golden %s through write (lint)' % gold_name, False, str(x))

        # ---- 2. planted refusals: (name, request, text the refusal must hold)
        planted = [
            # S109 ENT: attire + blood=on and entrances=off + blood=on are SERVED now (the builder section below); what stays refused on the builder:
            ('an entrance moment with entrances=off on the builder', 'scene Cena vs Lesnar; entrances=off; blood=on\nat entrance:0: screenshot a', 'no entrance plays'),
            ('a CAW attire above 3 (the builder\'s limits)', 'scene #5/4 vs #343; blood=on', 'CAW'),
            ('unknown name lists candidates', 'scene Brock Lesnarr vs Cena', 'did you mean'),
            ('a DLC name without dlc=on', 'scene Fandango vs Daniel Bryan', 'dlc=on'),
            ('player seat out of range', 'scene Cena vs Lesnar; player=5', 'seat'),
            # S110 SCENEMOMENTS: pin / count / kickout / move / replay ... are served now (moments_section); what stays refused:
            ('@remaining is bus only (no lowering yet)', 'scene Cena vs Lesnar\nat remaining: screenshot a', 'bus only'),
            ('eliminated in a one-on-one (no type=)', 'scene Cena vs Lesnar\nat eliminated: screenshot a', 'type='),
            ('finish:pinfall is written finish=NAME', 'scene Cena vs Lesnar; blood=on\nat finish:pinfall: screenshot a', 'finish=NAME'),
            ('finish=NAME with a name the bus does not print', 'scene Cena vs Lesnar\nat finish=knockout: screenshot a', 'pinfall | count_out'),
            ('a table break with no type= (no table in a plain match)', 'scene Cena vs Lesnar\nat table_break: screenshot a', 'type=68'),
            ('a ring break with no director line of the request\'s own', 'scene Cena vs Lesnar\nat ring_break: screenshot a', 'no recipe reaches'),
            ('a ring break whose director lines come only after it', 'scene Cena vs Lesnar\nat ring_break: screenshot a\nat finish: momentum 0 100', 'earlier position'),
            ('count=4', 'scene Cena vs Lesnar\nat count=4: screenshot a', '1..3'),
            ('move class=normal', 'scene Cena vs Lesnar\nat move:class=normal: screenshot a', 'events_moves=all'),
            ('a field the moment does not have', 'scene Cena vs Lesnar\nat pin:class=finisher: screenshot a', 'not a field'),
            ('=VALUE on a state moment', 'scene Cena vs Lesnar\nat bell=2: screenshot a', 'only a bus moment'),
            ('a bus word as an item', 'scene Cena vs Lesnar\nat bell: pin', 'not an item'),
            ('the highlight replay is not offered', 'scene Cena vs Lesnar; blood=on\nat highlight: screenshot a', 'not offered'),
            ('bleed without blood=on', 'scene Cena vs Lesnar\nat bell: bleed 0 1', 'blood=on'),
            ('an entrance moment with entrances=off', 'scene Cena vs Lesnar; entrances=off\nat entrance:0: screenshot a', 'no entrance plays'),
            ('a shot at a frame count after an entrance (the lint\'s own reason)', 'scene Cena vs Lesnar\nat entrance:0:1+120f: screenshot a', 'fast-forward moves a shot'),
            ('lines out of order (offset back)', 'scene Cena vs Lesnar\nat bell+300f: screenshot a\nat bell+100f: screenshot b', 'in order'),
            ('moments out of order', 'scene Cena vs Lesnar; blood=on\nat finish: screenshot a\nat bell: screenshot b', 'in order'),
            ('an unknown scene field', 'scene Cena vs Lesnar; weather=rain', 'unknown field'),
            ('type is needed for three wrestlers', 'scene Cena vs Lesnar vs Undertaker', 'type='),
            ('pace without type on the launcher', 'scene Cena vs Lesnar; pace=quick', 'type='),
            ('run timeout is not a route field', 'scene Cena vs Lesnar\nrun timeout=600', 'WaitSeconds'),
            ('run intent=watch', 'scene Cena vs Lesnar\nrun intent=watch', 'watch'),
            ('a run flag the scene sets itself', 'scene Cena vs Lesnar\nrun flags=--wwe2k14_fixed_seed=2', 'already set'),
            ('a bad action argument', 'scene Cena vs Lesnar\nat bell: momentum 0 400', 'momentum SLOT PCT'),
            ('save= differs from -Data', 'scene Cena vs Lesnar; save=blood_on', 'pass -Data'),
        ]
        for name, req, want in planted:
            try:
                S.lower(req, data='userdata_frozen') if 'save=' in req else S.lower(req)
                check('refused: ' + name, False, 'the request was served (lowered without a refusal)')
            except S.Refused as x:
                check('refused: ' + name, want.lower() in str(x).lower(), 'refused, but the reason lacks %r: %s' % (want, x))
        # the lint's R2 refusal also comes through write(): a hand-built route with the same shape is refused by the same text
        # an exe without wwe2k14_match is refused for the builder; one that holds it is served
        os.makedirs(os.path.join(tmp, 'bad'))   # route_lint.exe_knows caches by exe FOLDER: one folder each
        os.makedirs(os.path.join(tmp, 'good'))
        exe_bad, exe_ok = os.path.join(tmp, 'bad', 'bad.exe'), os.path.join(tmp, 'good', 'ok.exe')
        with open(exe_bad, 'wb') as fh:
            fh.write(b'MZ\0wwe2k14_launch\0wwe2k14_launch_participants\0')
        with open(exe_ok, 'wb') as fh:
            fh.write(b'MZ\0wwe2k14_match\0wwe2k14_match_go\0wwe2k14_launch\0')
        try:
            S.lower('scene Cena vs Lesnar; blood=on', exe=exe_bad)
            check('refused: an exe without wwe2k14_match (builder)', False, 'served')
        except S.Refused as x:
            check('refused: an exe without wwe2k14_match (builder)', 'wwe2k14_match' in str(x), str(x))
        try:
            S.lower('scene Cena vs Lesnar; blood=on', exe=exe_ok)
            check('an exe that holds wwe2k14_match is served', True)
        except S.Refused as x:
            check('an exe that holds wwe2k14_match is served', False, str(x))
        try:
            S.lower('scene Cena vs Lesnar\nrun exe=' + exe_bad.replace('\\', '/'), exe=exe_ok)
            check('refused: run exe= differing from the -Exe given', False, 'served')
        except S.Refused as x:
            check('refused: run exe= differing from the -Exe given', 'same exe' in str(x), str(x))

        # ---- pads: every seat a CPU unless `player=` names it (run 11's class: a positional token made a CPU into pad 1)
        t1, _, _ = S.lower('scene Cena vs Lesnar; blood=on')
        t2, _, _ = S.lower('scene Cena vs Lesnar; blood=on; player=1')
        t3, _, _ = S.lower('scene Cena vs Lesnar; entrances=off')
        t4, _, _ = S.lower('scene Cena vs Lesnar; entrances=off; player=0,1')
        check('pads: no player= gives every seat pad -1 (builder)', 'p=139:-1,343:-1' in t1, '')
        check('pads: player=1 gives seat 1 pad 0, seat 0 pad -1', 'p=139:-1,343:0' in t2, '')
        check('pads: launcher without player= names no pad', 'launch_participants=139,343 ' in t3 + ' ' or re.search(r'launch_participants=139,343(\s|$)', t3) is not None, '')
        check('pads: launcher player=0,1 gives pads 0 and 1', re.search(r'launch_participants=139:0,343:1(\s|$)', t4) is not None, '')

        # ---- S109 ENT: the builder serves attire and entrances=off (each twin fails on the pre-ENT scene.py: "no door serves both")
        ent_req = 'scene Cena/2 vs Lesnar; arena=5; blood=on; entrances=off\nat bell+120f: passive 1 on\nat finish:\nat victory+600f: shot face 0 v1'
        for name, req, want_in, want_out in (
                ('blood=on + entrances=off -> the builder with entrances=0', 'scene Cena vs Lesnar; entrances=off; blood=on',
                 ['cvar wwe2k14_match type=0;p=139:-1,343:-1;o=81:1;entrances=0'], ['--wwe2k14_launch', 'skip_entrances']),
                ('blood=on + an attire -> the builder with ID:PAD:N', 'scene #139/2 vs #343; blood=on', ['p=139:-1:2,343:-1;'], ['--wwe2k14_launch', 'entrances=0']),
                ('blood + no entrances + attire in one builder spec', ent_req, ['p=139:-1:2,343:-1;arena=5;o=81:1;entrances=0', '# door: builder'], ['--wwe2k14_launch'])):
            try:
                tb, _, _ = S.lower(req)
                miss = [w for w in want_in if w not in tb]
                extra = [w for w in want_out if w in tb]
                check('served: ' + name, not miss and not extra, 'missing %s, unwanted %s' % (miss, extra))
                if req == ent_req:
                    generated.append(S.write(req, out_dir=tmp))
            except S.Refused as x:
                check('served: ' + name, False, 'refused: %s' % x)
        # an exe without the ENT builder strings is refused for those requests (it would refuse the spec in the game); one with them is served
        os.makedirs(os.path.join(tmp, 'ent_old'))
        os.makedirs(os.path.join(tmp, 'ent_new'))
        exe_old, exe_new = os.path.join(tmp, 'ent_old', 'old.exe'), os.path.join(tmp, 'ent_new', 'new.exe')
        with open(exe_old, 'wb') as fh:
            fh.write(b'MZ\0wwe2k14_match\0wwe2k14_match_go\0')
        with open(exe_new, 'wb') as fh:
            fh.write(b'MZ\0wwe2k14_match\0wwe2k14_match_go\0' + getattr(S, 'ENT_MARK_ENTRANCES', '(absent)').encode() + b'\0' + getattr(S, 'ENT_MARK_ATTIRE', '(absent)').encode() + b'\0')
        for req, mark in (('scene Cena vs Lesnar; entrances=off; blood=on', 'entrances'), ('scene #139/2 vs #343; blood=on', 'attire')):
            try:
                S.lower(req, exe=exe_old)
                check('refused: %s on a pre-ENT exe' % mark, False, 'served')
            except S.Refused as x:
                check('refused: %s on a pre-ENT exe' % mark, 'S109 ENT' in str(x), str(x))
            try:
                S.lower(req, exe=exe_new)
                check('served: %s on an ENT exe' % mark, True)
            except S.Refused as x:
                check('served: %s on an ENT exe' % mark, False, str(x))
        try:   # a plain blood request needs no ENT string: an older exe still serves it (nothing changed for it)
            S.lower('scene Cena vs Lesnar; blood=on', exe=exe_old)
            check('a plain blood request still runs on a pre-ENT exe', True)
        except S.Refused as x:
            check('a plain blood request still runs on a pre-ENT exe', False, str(x))
        # the game side the builder door relies on (src\scenario.h, src\build_spec.h): pinned as text, the order is the point
        scen = open(os.path.join(ROOT, 'src', 'scenario.h'), encoding='utf-8').read()
        spec_h = open(os.path.join(ROOT, 'src', 'build_spec.h'), encoding='utf-8').read()
        hook = scen[scen.find('REX_HOOK_RAW(sub_822CBD28) {'):]
        hook = hook[:hook.find('\n}') + 2]
        fb = scen[scen.find('inline void FireBuild('):scen.find('inline void BuildTick(')]
        # S110 CVARSHARD moved every cvar definition (its help text with it) out of the headers into src\cvars\cvars_N.cpp (tools\compile\cvar_shard.py
        # picks the shard): the help the exe carries is the wwe2k14_match definition's string literals, joined, wherever that definition lives.
        match_defs = []
        cvars_dir = os.path.join(ROOT, 'src', 'cvars')
        for shard in sorted(n for n in os.listdir(cvars_dir) if re.fullmatch(r'cvars_\d+\.cpp', n)):
            text = open(os.path.join(cvars_dir, shard), encoding='utf-8').read()
            match_defs += [''.join(re.findall(r'"((?:[^"\\]|\\.)*)"', d)) for d in re.findall(r'REXCVAR_DEFINE_STRING\(wwe2k14_match, (.*?)\);\n', text, re.S)]
        match_help = match_defs[0] if len(match_defs) == 1 else ''
        pins = [
            ('one REX_HOOK_RAW(sub_822CBD28)', scen.count('REX_HOOK_RAW(sub_822CBD28) {') == 1),
            ('the hook runs the original when not armed (one branch, then __imp__)', hook.count('if (') == 1 and '__imp__sub_822CBD28(ctx, base);' in hook
             and hook.index('return;') < hook.index('__imp__sub_822CBD28')),
            ('the flag is thread_local', 'inline thread_local bool g_build_no_entrances' in scen),
            ('the flag is set only around FireBuild\'s finalize call', len(re.findall(r'^\s+g_build_no_entrances = ', scen, re.M)) == 2 and
             fb.index('g_build_no_entrances = !spec.entrances;') < fb.index('call(sub_829263B0, {s});') < fb.index('g_build_no_entrances = false;')),
            ('the seat write comes after sub_829278C8 and before finalize', fb.index('call(sub_829278C8, {s, i});') < fb.index('put32(rec + 20,') < fb.index('call(sub_829263B0')),
            ('one wwe2k14_match definition in src\\cvars, with help text', len(match_defs) == 1 and len(match_help) > 100),
            ('the exe strings scene.py checks are in the wwe2k14_match help', getattr(S, 'ENT_MARK_ENTRANCES', '(absent)') in match_help
             and getattr(S, 'ENT_MARK_ATTIRE', '(absent)') in match_help),
            ('ParseBuildSpec lives in build_spec.h only', 'inline bool ParseBuildSpec(' in spec_h and 'inline bool ParseBuildSpec(' not in scen),
        ]
        for name, ok in pins:
            check('game side: ' + name, ok)

        # ---- SCENE review twins (each fails on the builder's 767b9e4cd)
        try:   # lightsout is a dev verb, not a blood verb (src\situation_director_core.h, input_script.h)
            S.lower('scene Cena vs Lesnar\nat bell: lightsout 0')
            check('lightsout without blood=on is served', True)
        except S.Refused as x:
            check('lightsout without blood=on is served', False, str(x))
        try:   # victory with no finish line: the finish is waited first (S108 natural finishes took 3,500-6,500 frames after the bell; WINNING opens 1,100-1,700 after the finish)
            tv, _, _ = S.lower('scene Cena vs Lesnar; blood=on\nat victory: screenshot v')
            sv = semantic(tv)
            check('victory alone waits the finish word before WINNING', sv.index('wait_state 0x8376ACCC !=0 72000 2') < sv.index('wait_file WINNING 6000'), str(sv))
        except (S.Refused, ValueError) as x:
            check('victory alone waits the finish word before WINNING', False, str(x))
        try:   # every Nf xK = K shots: the wait outlasts the K-th periodic shot by one frame (fandango_expected, difference 2)
            te, _, _ = S.lower('scene Cena vs Lesnar; entrances=off\nat bell: every 30f x4 ev\nat bell+200f: screenshot after')
            se = semantic(te)
            i = se.index('screenshot_every 30 ev dup')
            check('every 30f x4: wait 121 then stop, the next offset counts from there', se[i + 1:i + 5] == ['wait_frames 121', 'screenshot_every 0', 'wait_frames 79', 'screenshot after'], str(se[i:]))
        except (S.Refused, ValueError) as x:
            check('every 30f x4: wait 121 then stop, the next offset counts from there', False, str(x))
        try:   # save= with no -Data / --data cannot be honoured by a route: refused, never dropped
            S.lower('scene Cena vs Lesnar; save=blood_on\nat bell: screenshot a')
            check('refused: save= with no -Data given', False, 'served (the save field was dropped)')
        except S.Refused as x:
            check('refused: save= with no -Data given', '-data' in str(x).lower(), str(x))

        # ---- no field silently dropped: every value of a full launcher request is in the lowered route
        full = 'scene #385 vs Daniel Bryan/3; dlc=on; arena=53; type=one on one > normal; pace=quick; entrances=off; player=1\nat bell+10f: screenshot z'
        try:
            tf, _, _ = S.lower(full)
            need = ['--wwe2k14_dlc=true', '--wwe2k14_launch_arena=53', '--wwe2k14_quick_type=0', '--wwe2k14_quick_pace=1', '--wwe2k14_launch_skip_entrances=true',
                    '--wwe2k14_launch_participants=385,324:0:3', '--wwe2k14_launch=@quick']
            miss = [n for n in need if n not in tf]
            check('every field of a full launcher request is in the route', not miss, 'missing: %s' % miss)
        except S.Refused as x:
            check('every field of a full launcher request is in the route', False, str(x))

        # ---- 5. S110 SCENEMODES: the match lowering pinned byte-identical; the modes
        for sha, req in PINS:
            try:
                got = hashlib.sha1(S.lower(req)[0].encode('utf-8')).hexdigest()
                check('pinned byte-identical: ' + req[:70], got == sha, 'sha1 %s, pinned %s' % (got, sha))
            except S.Refused as x:
                check('pinned byte-identical: ' + req[:70], False, 'refused: %s' % x)
        modes_section(S, tmp, generated)
        moments_section(S, tmp, generated)

        # ---- 3 / 4. every generated route lints; the item word files on the board
        extra = [
            'scene Cena vs Lesnar; blood=on\nat bell+120f: passive 1 on\nat finish:\nat victory+600f: shot face 0 v1',
            'scene Cena vs Lesnar; blood=on\nat entrance:0:5: screenshot e0\nat bell+60f: screenshot b0',
            'scene Cena vs Lesnar; entrances=off; arena=5\nat clock:600: record r1\nat clock:900: every 30f x4 ev',
            'scene Cena vs Lesnar; type=one on one > normal; entrances=off\nat roster: log match_clock',
        ]
        for r in extra:
            try:
                generated.append(S.write(r, out_dir=tmp))
            except S.Refused as x:
                check('generated: ' + r.splitlines()[-1], False, str(x))
        import coverage
        for p in generated:
            errs, warns, _f, _h = route_lint.lint_route(p, ROOT, final=' '.join(route_lint.header_flags(open(p, encoding='utf-8').read())))
            check('lint 0 ERROR: ' + os.path.basename(p), not errs, '; '.join(errs))
            name = os.path.basename(p)
            word = name.split('_')[0]
            item = next((i for i, rx in coverage.ITEM_RULES if re.search(rx, name, re.I)), None)
            want = {'victory': 'victory', 'entrance': 'entrances', 'match': 'matches', 'wm30': 'wm30', 'create': 'create', 'menu': 'menus'}[word]
            check('board item of %s = %s' % (name, want), item == want, 'files as %s' % item)
            body = open(p, encoding='utf-8').read()
            check('first step logs the request: ' + name, [ln for ln in body.splitlines() if ln and not ln.startswith('#') and not ln.startswith('on_fail')][0].startswith('log bench: request '), '')
            check('no watch / performance term: ' + name, not re.search(r'^# intent:.*\b(watch|performance)\b', body, re.M), '')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print('\n%d passed, %d FAILED' % (len(PASSES), len(FAILS)))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
