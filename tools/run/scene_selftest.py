r"""S109 SCENE selftest (no game, no window): `py -I tools\run\scene_selftest.py [--scene PATH]`

1. Six golden lowerings: a scene request lowered by tools\run\scene.py must give, step by step, the semantic lines of the S108 route it replaces
   (tools\input_scripts\scenes\s108_*.txt): bld7_bloodon (builder + bleeds + face shots), shot_band, hair_match (the fixed `103::2`), fandango_entrance,
   caa_match, ban_arena53. Semantic = the route's steps without comments, `log`, `mark`, `include`, `on_fail`, the match_go number and the closing wait + quit;
   the launch flags are compared as sets. The two deliberate differences (fandango: `every` instead of 14 hand-copied shots, BG04 wait not written) are stated below.
2. Planted refusals: each must raise Refused with its reason (a stub that never refuses fails every one of them: `--scene STUB.py`).
3. Every generated route passes tools\bench\route_lint.py with 0 ERROR fast-forwarded (no watch / performance term).
4. Routes file on the coverage board under their item word.
`--scene PATH` loads another module with the same names (lower, write, Refused): how a stub is shown to fail the planted cases.
"""
import importlib.util
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
    return sem[:a] + ['screenshot_every 40 sc_fan', 'wait_frames 561', 'screenshot_every 0'] + sem[b + 1:]


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
            ('@pin is bus only', 'scene Cena vs Lesnar\nat pin: screenshot a', 'bus only'),
            ('@count is bus only', 'scene Cena vs Lesnar\nat count: screenshot a', 'bus only'),
            ('@kickout is bus only', 'scene Cena vs Lesnar\nat kickout: screenshot a', 'bus only'),
            ('@eliminated is bus only', 'scene Cena vs Lesnar\nat eliminated: screenshot a', 'bus only'),
            ('a finisher (move) is bus only', 'scene Cena vs Lesnar\nat move: screenshot a', 'bus only'),
            ('finish:pinfall is bus only', 'scene Cena vs Lesnar; blood=on\nat finish:pinfall: screenshot a', 'bus only'),
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
        pins = [
            ('one REX_HOOK_RAW(sub_822CBD28)', scen.count('REX_HOOK_RAW(sub_822CBD28) {') == 1),
            ('the hook runs the original when not armed (one branch, then __imp__)', hook.count('if (') == 1 and '__imp__sub_822CBD28(ctx, base);' in hook
             and hook.index('return;') < hook.index('__imp__sub_822CBD28')),
            ('the flag is thread_local', 'inline thread_local bool g_build_no_entrances' in scen),
            ('the flag is set only around FireBuild\'s finalize call', len(re.findall(r'^\s+g_build_no_entrances = ', scen, re.M)) == 2 and
             fb.index('g_build_no_entrances = !spec.entrances;') < fb.index('call(sub_829263B0, {s});') < fb.index('g_build_no_entrances = false;')),
            ('the seat write comes after sub_829278C8 and before finalize', fb.index('call(sub_829278C8, {s, i});') < fb.index('put32(rec + 20,') < fb.index('call(sub_829263B0')),
            ('the exe strings scene.py checks are in the wwe2k14_match help', getattr(S, 'ENT_MARK_ENTRANCES', '(absent)') in scen and getattr(S, 'ENT_MARK_ATTIRE', '(absent)') in scen),
            ('ParseBuildSpec lives in build_spec.h only', 'inline bool ParseBuildSpec(' in spec_h and 'inline bool ParseBuildSpec(' not in scen),
        ]
        for name, ok in pins:
            check('game side: ' + name, ok)

        # ---- SCENE review twins (each fails on the builder's 767b9e4cd)
        try:   # lightsout is a dev verb, not a blood verb (src\situation_director_core.h:119-123, input_script.h:1370)
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
            i = se.index('screenshot_every 30 ev')
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
            want = {'victory': 'victory', 'entrance': 'entrances', 'match': 'matches'}[word]
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
