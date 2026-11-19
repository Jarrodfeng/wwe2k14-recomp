#!/usr/bin/env python3
r"""Feature diff (Session 103, lane T2): WHAT IS DRAWN, game vs ours, per frame and per class. 0 game runs, 0 Surface runs; offline.

  py -I tools\scene\feature_diff.py CASE.zip [--frames 400,500] [--out DIR] [--env KEY=VAL ...] [--json OUT.json] [--max-values 12]
  py -I tools\scene\feature_diff.py --selftest          (the tool's own check: synthetic faults on a real case frame; the gate lane s103_T2 runs it)

Why: the census checks INPUTS ("is this value read and stamped") and the mirror sheet's difference image is one blended number. Nothing compared WHAT IS DRAWN, so whole features
(the crowd lit, the barricade's alpha_spec variant, the wrestlers' shadows, rim lights, depth of field) went missing while every check passed. This tool lists them.

Three layers, game side read in Python from the case's own stream (tools\solver\capread.py + the fx.json hashes), our side = the case's scene record drawn by the offline rig
(out\tools\frame_rig.exe, WWE_RDR_FEATURE_DUMP=file: one JSON line per draw our front-end submitted; the rig's report for the maps; a "pass" line for the post chain):
  1 PASSES    what the game ran (its techniques: shadow map, plane shadow, circle shadow, rim map, sweat map, DOF, bloom, glare, grade) and whether our frame ran an equivalent.
  2 VARIANTS  per class (arena / props / wrestler / crowd) the vertex / pixel variants and their FX_* defines (FX_LIT, FX_SHADOW, FX_RIM*, FX_ALPHA_SPEC, FX_LM*, ...) drawn, and draw counts.
  3 VALUES    per texture slot what is bound (a real map / white / black / neutral / missing) and every named constant the draws are fed (DifLghCol / DifLghVec / Amb / Sky / Ground,
              ShadowPrm, TexProjLight, MatAmbCol / MatDifCol, the rim / sweat params ...): a game value no draw of ours is fed (beyond a per-constant tolerance) is reported, worst first.
Output per frame: a short table + a MISSING list (worst first) + the VALUES list; the case summary merges the frames. Exit 0 always for a report run (it reports; the gate lane checks it).
Game bytes are read in place (the zip); nothing is written but our own numbers (--out under out\).
"""
import argparse, collections, json, math, os, re, struct, subprocess, sys, zipfile
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'solver'))
import capread  # noqa: E402
import solve    # noqa: E402

FX_DIR = os.path.join(ROOT, 'src', 'renderer', 'shaders', 'fx')
ROLES = {0: 'unnamed', 1: 'SceneColor', 2: 'SceneDepth', 3: 'ShadowMap', 4: 'PostStep', 5: 'FeedbackMap', 6: 'AtlasTarget', 7: 'MenuPicture', 8: 'TronPicture', 9: 'Ui', 10: 'Present', 11: 'Other', 12: 'RimMap'}
FLAT = {'white', 'black', 'grey', 'neutral_normal', 'black_cube', 'none'}
FAMILIES = ['arena', 'props', 'wrestler', 'crowd']
# per-draw noise: never a "feature" (placement, bones, time, per-mesh uv / scroll); the rest of a variant's constants are compared by name
NOISE = re.compile(r'(^|_)(mWVP|WorldMatrix|World|Bone|Bools|ViewProj|CameraViewProj|View|EyePos|Time|UVScroll\d|Scroll|LightMapParam|MipmapBias|UvOffset|Offset|Palette|g_f2SampleOffsets|TexMat|Color|ChangeColor)', re.I)
TOL = {'default': (0.02, 0.03)}   # (absolute, relative) of the larger value; a per-constant override goes below
for name in ('DifLghVec', 'SkyDir', 'KeyLightVec'):
    TOL[name] = (0.05, 0.0)
for name in ('ShadowPrm', 'TexProjLight', 'TexProjLightXY', 'TexProjLightW', 'ViewLight', 'PlaneInv', 'RimLightDepth', 'ConvScreen'):
    TOL[name] = (0.5, 0.05)
# pass table: feature -> (the game technique names, how ours says it ran)
GAME_PASSES = [
    ('shadow map (casters)', ('T_ShadowMap', 'T_ShadowMapBlur'), 'shadow'),
    ('plane shadow', ('T_PlaneShadowMap', 'T_PlaneShadowBlur'), 'plane'),
    ('circle shadow', ('T_CircleShadow',), 'circles'),
    ('rim light map', ('T_RimLightMap',), 'rim'),
    ('sweat / liquid map', ('T_Paste',), 'sweat'),
    ('pre-light map', ('T_PreLight', 'T_PreLightMap'), 'prelight'),
    ('depth of field', ('T_DownScale2x2_DOF',), 'dof'),
    ('bloom', ('T_Bloom',), 'bloom'),
    ('glare / star', ('T_Star',), 'glare'),
    ('grade / final combine', ('T_Final',), 'post'),
    ('particles (sprites / flares)', ('particle draws',), 'particles'),   # S103 F5: the stream's particle draws are origin class 6 (their technique name is empty in the captures)
]


def family_of(effect):
    e = effect or ''
    if e.startswith('yAud'): return 'crowd'
    if e.startswith('yBG_'): return 'arena'
    if e.startswith('yProp_'): return 'props'
    if e.startswith('yCh_'): return 'wrestler'
    if e.startswith(('yShadowMap', 'yPlaneShadow', 'yCircleShadow', 'yRimLightMap', 'yLiquidMap')): return 'maps'
    if e.startswith(('yBlur', 'yDownScale', 'yGlareEtc', 'yMonotone', 'yAfterImage', 'yAntialiasing')): return 'post'
    return 'other'


class Fx:
    """variant name <-> defines, samplers, from the fx.json files; plus solve.Effects for hash resolution."""
    def __init__(self):
        self.eff = solve.Effects()
        self.defines = {}   # '<effect>_<variant>_<stage>' -> [defines]
        self.samplers = {}
        for fn in os.listdir(FX_DIR):
            if not fn.endswith('.fx.json'):
                continue
            d = json.load(open(os.path.join(FX_DIR, fn), encoding='utf-8'))
            for stage in ('vertex', 'fragment'):
                for v in d.get(stage, []):
                    nm = '%s_%s_%s' % (d['effect'], v['name'], stage)
                    self.defines[nm] = list(v.get('defines', []))
                    self.samplers[nm] = [s['slot'] for s in v.get('samplers', [])]

    def name_of(self, stage, h, prefer=None):
        c = self.eff.by_hash[stage].get(h, [])
        if not c:
            return None
        if prefer:
            for e, vn in c:
                if e == prefer:
                    return '%s_%s_%s' % (e, vn, stage)
        e, vn = c[0]
        return '%s_%s_%s' % (e, vn, stage)

    def effect_of(self, variant):
        return variant.rsplit('_', 2)[0] if variant else ''


# ------------------------------------------------------------------------------------------------ the game side
class Case:
    def __init__(self, path):
        self.path = path
        self.src = capread.Source(path)
        self.zf = self.src.zf
        self.dir = None if self.zf else str(self.src.path)   # a capture folder (frames\, scene\, images.bin) reads like a case zip's capture\
        if self.zf:
            names = self.zf.namelist()
        else:
            names = []
            for sub in ('scene', 'frames', ''):
                d = os.path.join(self.dir, sub)
                if os.path.isdir(d):
                    names += [(sub + '/' if sub else '') + f for f in os.listdir(d)]
        self.scene = {int(re.search(r'frame_(\d+)', n).group(1)): n for n in names if n.endswith('.rsc')}
        self.frame_names = {int(re.search(r'frame_(\d+)', n).group(1)): n for n in self.src.frames}
        self.images = None
        nm = next((n for n in names if n.endswith('images.bin')), None)
        if nm:
            b = self.read(nm)
            cnt = struct.unpack_from('<I', b, 8)[0]
            self.images = {}
            for i in range(cnt):
                o = 16 + 24 * i
                iid, w, h, d = struct.unpack_from('<IHHH', b, o)
                role = b[o + 19] if o + 19 < len(b) else 0
                self.images[iid] = dict(w=w, h=h, role=ROLES.get(role, str(role)))
        self.manifest = self.src.manifest or {}


def _read(self, name):
    return self.zf.read(name) if self.zf else open(os.path.join(self.dir, name), 'rb').read()


Case.read = _read


def game_frame(case, fx, n):
    fr = capread.Frame(case.src.read(case.frame_names[n]))
    draws, passes = [], collections.Counter()
    for i in range(len(fr.draws)):
        d = fr.draws[i]
        o = fr.origins[i] if fr.has_origins else None
        tech = fr.string(int(o['technique'])) if o is not None else ''
        klass = solve.KLASS.get(int(o['klass']), '?') if o is not None else 'unnamed'
        lab, per_stage, why = fx.eff.resolve(int(d['vs_hash']), int(d['ps_hash']))
        pe = lab.split('|')[0] if lab else None
        vs = fx.name_of('vertex', int(d['vs_hash']), pe)
        ps = fx.name_of('fragment', int(d['ps_hash']), pe) if int(d['ps_hash']) else None
        eff = fx.effect_of(ps or vs)
        passes[tech or '(no technique)'] += 1
        if klass == 'particle':
            passes['particle draws'] += 1   # S103 F5
        slots = {}
        for s in fr.slots[int(d['tex_first']):int(d['tex_first']) + int(d['tex_count'])]:
            im = int(s['image'])
            info = case.images.get(im) if case.images else None
            kind = 'none' if im == 0 else ('target:' + info['role'] if info and info['role'] not in ('unnamed', 'Other') else 'map')
            slots[int(s['slot'])] = kind
        vals = {}
        for stage, bank_i in (('vertex', int(d['vs_bank'])), ('fragment', int(d['ps_bank']))):
            for (field, reg, cnt) in per_stage.get(stage, []):
                if cnt > 16 or bank_i >= len(fr.banks) or NOISE.search(field):
                    continue
                vals[field] = [float(x) for x in fr.reg(bank_i, reg, cnt).reshape(-1)]
        draws.append(dict(klass=klass, tech=tech, vs=vs, ps=ps, effect=eff, family=family_of(eff) if klass == 'model_part' or klass == 'crowd' else klass, slots=slots, vals=vals))
    return fr, draws, passes


def stream_camera(case, fx, fr):
    """The game-space row-vector world -> clip matrix of the stream's first arena draw (ViewProj = VS c56..c59) and the eye solved from it: the same camera frame_rig.py borrows."""
    for i in range(len(fr.draws)):
        d = fr.draws[i]
        lab, per_stage, _ = fx.eff.resolve(int(d['vs_hash']), int(d['ps_hash']))
        if not lab or not lab.startswith('yBG_'):
            continue
        for (field, reg, cnt) in per_stage.get('vertex', []):
            if field == 'ViewProj' and cnt == 4:
                M = fr.reg(int(d['vs_bank']), reg, 4).astype(np.float64)
                if np.isfinite(M).all() and abs(M).max() > 0:
                    A, b = M[[0, 1, 3], :3], -M[[0, 1, 3], 3]
                    eye = np.linalg.solve(A, b)
                    return M.T.astype(np.float32).reshape(-1), eye.astype(np.float32)
    return None, None


# ------------------------------------------------------------------------------------------------ our side
def our_frame(case, fx, n, work, rig, game_dir, vp, eye, env_extra, tag='ours', extra_args=()):
    os.makedirs(work, exist_ok=True)
    rsc = os.path.join(work, 'frame_%06d.rsc' % n)
    open(rsc, 'wb').write(case.read(case.scene[n]))
    dump = os.path.join(work, '%s_%d_dump.jsonl' % (tag, n))
    if os.path.exists(dump):
        os.remove(dump)
    cmd = [rig, '--record', rsc, '--game', game_dir, '--size', os.environ.get('FEATURE_DIFF_SIZE', '1280x720'), '--prefix', os.path.join(work, '%s_%d' % (tag, n)), '--repeat', '1',
           '--stream-viewproj', ','.join('%.9g' % x for x in vp), '--camera', ','.join('%.9g' % x for x in vp), '--eye', ','.join('%.9g' % x for x in eye)]
    cmd += list(extra_args)
    env = dict(os.environ, WWE_RDR_FEATURE_DUMP=dump, RDR_MAIN_CHECKOUT=main_checkout())
    env.update(env_extra)
    p = subprocess.run(cmd, capture_output=True, text=True, errors='replace', env=env, timeout=1800)
    text = (p.stdout or '') + (p.stderr or '')
    rep = os.path.join(work, '%s_%d_report.txt' % (tag, n))
    if os.path.exists(rep):
        text += '\n' + open(rep, encoding='utf-8', errors='replace').read()
    draws, post = [], None
    if os.path.exists(dump):
        begins = 0
        for line in open(dump, encoding='utf-8'):
            try:
                j = json.loads(line)
            except Exception:
                continue
            if j.get('k') == 'pass' and j.get('name') == 'frame_begin':
                begins += 1   # the rig records the whole frame first, then one panel per family: only the first is the frame
                continue
            if j.get('k') == 'draw' and begins <= 1:   # S103 P2: later blocks (the seed frame, a family panel) add no draws, but the whole panel's post line can come after them
                j['effect'] = fx.effect_of(j['ps'] or j['vs'])
                j['family'] = family_of(j['effect'])
                draws.append(j)
            elif j.get('k') == 'pass' and j.get('name') == 'post':
                post = j
    return dict(rc=p.returncode, text=text, draws=draws, post=post)


def ours_passes(o):
    """feature id -> (ran, why) from the rig's own report lines and the dump's pass line."""
    t = o['text']
    res = {}
    m = re.search(r'maps_made (\d+) shadow_skip "([^"]*)" circles_skip "([^"]*)"', t)
    if m:
        res['shadow'] = (not m.group(2), m.group(2) or 'made')
        res['circles'] = (not m.group(3), m.group(3) or 'made')
    else:
        res['shadow'] = (False, 'the rig report has no maps line (no maps hook ran)')
        res['circles'] = (False, 'the rig report has no maps line')
    e2 = re.search(r'\[scene\]\[maps\]\[e2\] (rim made .*)$', t, re.M)
    if e2:
        line = e2.group(1)
        for key, pat in (('rim', r'rim made (\d+) \((.*?)\), plane made'), ('plane', r'plane made (\d+) \((.*?)\), pre-light made'),
                         ('prelight', r'pre-light made (\d+) \((.*?)\), sweat made'), ('sweat', r'sweat made (\d+) \((.*)')):
            m2 = re.search(pat, line)
            res[key] = (bool(m2) and int(m2.group(1)) > 0, (m2.group(2)[:160] if m2 else 'unparsed [scene][maps][e2] line'))
    else:
        for k in ('rim', 'plane', 'prelight', 'sweat'):
            res[k] = (False, 'the rig report has no [scene][maps][e2] line')
    # S103 F5: the particle hook's census in the rig report: ran = it drew at least one particle draw (a fixture flare, or a record's tables)
    mp = re.search(r'\[scene\]\[particles\] frames (\d+), batches (\d+), particles (\d+), draws (\d+), stale (\d+), soft_as_hard (\d+), no_texture (\d+)', t)
    fx_line = re.search(r'^particle_fixture \(a FIXTURE[^\n]*', t, re.M)
    if mp:
        res['particles'] = (int(mp.group(4)) > 0, 'particle hook: batches %s particles %s draws %s no_texture %s%s' % (mp.group(2), mp.group(3), mp.group(4), mp.group(7), ' [a FIXTURE flare put in by the rig]' if fx_line else ''))
    else:
        res['particles'] = (False, 'the rig report has no particle hook census')
    p = o['post']
    if p:
        res['post'] = (True, 'post chain ran')
        res['dof'] = (bool(p['dof']), 'dof on' if p['dof'] else 'dof off in the post params')
        res['bloom'] = (bool(p['bloom']) and not p['dead_bloom'], 'bloom on' if p['bloom'] else 'bloom off in the post params')
        res['glare'] = (bool(p['glare']) and not p['dead_glare'], 'glare on' if p['glare'] else 'glare off in the post params')
    else:
        for k in ('post', 'dof', 'bloom', 'glare'):
            res[k] = (False, 'our frame recorded no post pass (the record has no post settings, or the frame did not reach it)')
    return res


# ------------------------------------------------------------------------------------------------ the diff
def tol_of(name, a, b):
    ab, rel = TOL.get(name, TOL['default'])
    return ab + rel * max(abs(a), abs(b))


def vec_dist(name, g, o):
    """(worst excess over the tolerance as a ratio, element index, game value, our value); lengths differ -> the common prefix."""
    n = min(len(g), len(o))
    worst = (0.0, 0, 0.0, 0.0)
    skip_w = bool(re.search(r'(Col|Color|Vec|Dir)$', name))   # a colour / direction's 4th component is padding (the game leaves 1 or garbage, ours 0)
    for i in range(n):
        if skip_w and i % 4 == 3:
            continue
        if name == 'SkyDir' and i % 4 == 1:   # our scene is in the viewer's mirrored space (the documented y flip, front_end.cpp Resolve SkyDir)
            g, o = list(g), list(o)
            g[i], o[i] = abs(g[i]), abs(o[i])
        d = abs(g[i] - o[i])
        r = d / max(tol_of(name, g[i], o[i]), 1e-9)
        if r > worst[0]:
            worst = (r, i, g[i], o[i])
    return worst


def diff(gdraws, gpasses, odraws, opass, fx):
    """-> dict(table, missing, values): everything as plain data."""
    out = dict(counts={}, missing=[], values=[], variants={}, passes=[])
    gc, oc = collections.Counter(d['family'] for d in gdraws), collections.Counter(d['family'] for d in odraws)
    for f in sorted(set(gc) | set(oc), key=lambda x: (x not in FAMILIES, x)):
        out['counts'][f] = (gc.get(f, 0), oc.get(f, 0))
    # 1 passes
    for feat, techs, key in GAME_PASSES:
        n = sum(gpasses.get(t, 0) for t in techs)
        if not n:
            continue
        ran, why = opass.get(key, (False, 'no way to tell'))
        out['passes'].append((feat, n, ran, why))
        if not ran:
            out['missing'].append(dict(rank=100 + min(n, 99) / 100.0, kind='PASS', family='-', what='pass "%s" ran in the game (%d draws), not in ours' % (feat, n), why=why, draws=n))
    pr = opass.get('particles')
    if pr and pr[0] and not gpasses.get('particle draws'):   # S103 F5: ours drew particles, the game's frame has none (a fixture): said, never counted as a match
        out['passes'].append(('particles (sprites / flares)', 0, True, pr[1]))
    # 2 variants + defines, per class
    for fam in FAMILIES:
        gv = collections.Counter((d['vs'], d['ps']) for d in gdraws if d['family'] == fam)
        ov = collections.Counter((d['vs'], d['ps']) for d in odraws if d['family'] == fam)
        out['variants'][fam] = dict(game=[('%s + %s' % k, n) for k, n in gv.most_common()], ours=[('%s + %s' % k, n) for k, n in ov.most_common()])
        gdef, odef = collections.Counter(), collections.Counter()
        for (vs, ps), n in gv.items():
            for nm in (vs, ps):
                for df in fx.defines.get(nm, []):
                    gdef[df] += n
        for (vs, ps), n in ov.items():
            for nm in (vs, ps):
                for df in fx.defines.get(nm, []):
                    odef[df] += n
        if sum(gv.values()) and not sum(ov.values()):
            out['missing'].append(dict(rank=90, kind='CLASS', family=fam, what='the game drew %d %s draws; ours drew none through the front-end' % (sum(gv.values()), fam), why='', draws=sum(gv.values())))
            continue
        for df in sorted(set(gdef) | set(odef)):
            g, o = gdef.get(df, 0), odef.get(df, 0)
            if g and not o:
                out['missing'].append(dict(rank=80 + min(g, 99) / 100.0, kind='DEFINE', family=fam, what='%s: the game uses %s on %d draws, ours on none' % (fam, df, g), why='', draws=g))
            elif o and not g:
                out['missing'].append(dict(rank=40 + min(o, 99) / 100.0, kind='EXTRA', family=fam, what='%s: ours uses %s on %d draws, the game on none' % (fam, df, o), why='', draws=o))
            elif g and o and (o / max(1, sum(ov.values()))) < 0.25 * (g / max(1, sum(gv.values()))):
                out['missing'].append(dict(rank=60 + min(g, 99) / 100.0, kind='DEFINE', family=fam, what='%s: %s is on %d of %d game draws, only %d of %d of ours' % (fam, df, g, sum(gv.values()), o, sum(ov.values())), why='', draws=g))
        gs, os_ = {d['vs'] for d in gdraws if d['family'] == fam} | {d['ps'] for d in gdraws if d['family'] == fam}, {d['vs'] for d in odraws if d['family'] == fam} | {d['ps'] for d in odraws if d['family'] == fam}
        gx = collections.Counter(d['ps'] or d['vs'] for d in gdraws if d['family'] == fam)
        ox = collections.Counter(d['ps'] or d['vs'] for d in odraws if d['family'] == fam)
        for nm, n in gx.most_common():
            if nm not in ox and nm in fx.defines and not any(f.startswith('FX') for f in []):
                out['missing'].append(dict(rank=30 + min(n, 99) / 100.0, kind='VARIANT', family=fam, what='%s: game pixel variant %s (%d draws) is not drawn by ours' % (fam, nm, n), why='', draws=n))
    # 3 values: slots and constants, per class
    for fam in FAMILIES:
        gd = [d for d in gdraws if d['family'] == fam]
        od = [d for d in odraws if d['family'] == fam]
        if not gd or not od:
            continue
        # slots: per slot the kinds each side binds
        slots = sorted({s for d in gd for s in d['slots']} | {int(s) for d in od for s in d['slots']})
        for s in slots:
            gk = collections.Counter(d['slots'].get(s, 'unbound') for d in gd)
            ok = collections.Counter(d['slots'].get(str(s), 'unbound') for d in od)
            gn, on = sum(v for k, v in gk.items() if k != 'unbound'), sum(v for k, v in ok.items() if k != 'unbound')
            oflat = sum(v for k, v in ok.items() if k in FLAT and k != 'unbound')
            if gn and on and oflat >= 0.5 * on and sum(v for k, v in gk.items() if k not in FLAT and k != 'unbound') >= 0.5 * gn:
                out['values'].append(dict(rank=50 + min(gn, 99) / 100.0, family=fam, name='slot %d' % s, what='%s slot %d: ours binds %s on %d of %d draws; the game binds %s (%d draws)' % (fam, s, 
                    '/'.join(sorted(k for k in ok if k in FLAT and k != 'unbound')), oflat, on, '/'.join(sorted(k for k in gk if k not in FLAT and k != 'unbound')), gn), draws=gn))
            elif gn and not on:
                out['values'].append(dict(rank=45 + min(gn, 99) / 100.0, family=fam, name='slot %d' % s, what='%s slot %d: the game samples it (%s) on %d draws; ours has no such slot' % (fam, s, '/'.join(sorted(gk)), gn), draws=gn))
        # constants
        names = sorted({k for d in gd for k in d['vals']} & {k for d in od for k in d['vals']})
        for nm in names:
            gvals = collections.Counter(tuple(round(x, 4) for x in d['vals'][nm]) for d in gd if nm in d['vals'])
            ovals = [list(k) for k in {tuple(round(x, 4) for x in d['vals'][nm]) for d in od if nm in d['vals']}]
            if not ovals:
                continue
            worst = None
            for gv_, n in gvals.items():
                same = [o for o in ovals if len(o) == len(gv_)]   # a vector of another length is another SHAPE (4 lights vs 5): never compared on its common prefix
                best = min((vec_dist(nm, list(gv_), o) for o in same), key=lambda r: r[0]) if same else (50.0, min(len(gv_), len(ovals[0])), gv_[0], ovals[0][0])
                if best[0] > 1.0 and (worst is None or best[0] * min(n, 20) > worst[0][0] * min(worst[1], 20)):
                    worst = (best, n, gv_)
            if worst:
                (r, idx, gval, oval), n, gv_ = worst
                out['values'].append(dict(rank=20 + min(r, 50) / 50.0 * 20, family=fam, name=nm, draws=n,
                                          what='%s %s%s: game %.4g, nearest of ours %.4g (%.1fx the tolerance; %d game draws carry it; ours has %d distinct values)' % (
                                              fam, nm, ('[%d].%s' % (idx // 4, 'xyzw'[idx % 4])) if len(gv_) > 4 else ('.' + 'xyzw'[idx % 4]), gval, oval, r, n, len(ovals))))
    out['missing'].sort(key=lambda m: -m['rank'])
    out['values'].sort(key=lambda m: -m['rank'])
    return out


def render(n, res, gstats, ostats):
    L = ['frame %d: game %s | ours %s' % (n, gstats, ostats)]
    L.append('  class        game  ours      note')
    for f, (g, o) in res['counts'].items():
        L.append('  %-10s %6d %5d      %s' % (f, g, o, 'NOT DRAWN' if g and not o and f in FAMILIES else ('(drawn through other paths: passes / 2D / hooks)' if f not in FAMILIES else '')))
    L.append('  passes the game ran (techniques) and ours:')
    for feat, n_, ran, why in res['passes']:
        L.append('    %-24s game %4d draws   ours %s%s' % (feat, n_, 'RAN' if ran else 'MISSING', ('  (' + why[:110] + ')') if (not ran or n_ == 0) else ''))
    L.append('  MISSING (worst first):')
    if not res['missing']:
        L.append('    (none)')
    for m in res['missing']:
        L.append('    %-7s %s' % (m['kind'], m['what']))
    L.append('  VALUES (what the matched draws are bound / fed; worst first):')
    if not res['values']:
        L.append('    (none)')
    for m in res['values'][:MAX_VALUES]:
        L.append('    %-4s %s' % ('SLOT' if m['name'].startswith('slot') else 'CONST', m['what'] if m['name'].startswith('slot') else m['what']))
    if len(res['values']) > MAX_VALUES:
        L.append('    ... %d more (--max-values)' % (len(res['values']) - MAX_VALUES))
    return '\n'.join(L)


MAX_VALUES = 12
BODY_SCALE_ONE = True   # S103 L14


def main_checkout():
    env = os.environ.get('RDR_MAIN_CHECKOUT')
    if env:
        return env
    common = subprocess.run(['git', '-C', ROOT, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True).stdout.strip()
    return os.path.dirname(common) if common else ROOT


def run_frames(case, fx, frames, out, envs, tag='ours', rig=None, look_from_stream=False):
    main = main_checkout()
    game_dir = os.path.join(main, 'game_data', 'extracted')
    rig = rig or os.path.join(ROOT, 'out', 'tools', 'frame_rig.exe')
    if not os.path.exists(rig):
        raise SystemExit('FAIL  no %s (build it: pwsh tools\\scene\\build_rig.ps1)' % rig)
    results = []
    for n in frames:
        if n not in case.frame_names or n not in case.scene:
            raise SystemExit('FAIL  frame %d: not in the case (stream frames %d..%d, records %s..%s)' % (n, min(case.frame_names), max(case.frame_names), min(case.scene or [0]), max(case.scene or [0])))
        fr, gdraws, gpasses = game_frame(case, fx, n)
        vp, eye = stream_camera(case, fx, fr)
        if vp is None:
            raise SystemExit('FAIL  frame %d: no arena draw with a ViewProj in the stream to take the camera from' % n)
        # S103 L14: a case from a live run has no body-scale table, so the rig drew NO wrestler and every wrestler-fed map (shadow, plane, rim, sweat) said "no caster is resident": told scale 1 here
        # ([U]: a rig-only fixture, the way l9_check does; a tool fault, not a missing record input)
        extra = ['--body-scale-one'] if BODY_SCALE_ONE else []
        if look_from_stream:   # an ENTRANCE capture whose record has no look rows: the rig's light rows are read from the stream's own constants (frame_rig.stream_look)
            sys.path.insert(0, HERE)
            import frame_rig
            extra, msg = frame_rig.stream_look(case.dir or case.path, n, out)
            print('look rows: ' + msg)
        o = our_frame(case, fx, n, out, rig, game_dir, vp, eye, envs, tag, extra)
        if o['rc'] != 0 or not o['draws']:
            raise SystemExit('FAIL  frame %d: the rig exit %s, %d draws dumped:\n%s' % (n, o['rc'], len(o['draws']), o['text'][-600:]))
        res = diff(gdraws, gpasses, o['draws'], ours_passes(o), fx)
        results.append((n, res, '%d draws' % len(gdraws), '%d draws' % len(o['draws']), gdraws, o))
    return results


def summary(results):
    """The whole-case summary: every MISSING / VALUES item once, with the frames it showed on, worst first."""
    agg = {}
    for n, res, *_ in results:
        for sect in ('missing', 'values'):
            for m in res[sect]:
                key = (sect, m.get('kind', 'VALUE'), m['what'].split(' (')[0].split(':')[0] + '|' + (m['name'] if 'name' in m else m['what'][:60]))
                e = agg.setdefault(key, dict(m, frames=[], sect=sect))
                e['frames'].append(n)
    rows = sorted(agg.values(), key=lambda e: -e['rank'])
    return rows


def selftest():
    """Synthetic faults on a REAL frame's game-side data: the 'ours' side is the game's own draws re-labelled as ours (a perfect copy: the diff must report nothing), then mutated
    one fault at a time. Each fault must be reported under its own token; the perfect copy must report none (a checker that finds everything finds nothing)."""
    fx = Fx()
    case_path = os.environ.get('FEATURE_DIFF_CASE') or os.path.join(main_checkout(), 'out', 's103', 'run27_case.zip')
    case = Case(case_path)
    n = 500 if 500 in case.frame_names else sorted(case.frame_names)[len(case.frame_names) // 2]
    fr, gdraws, gpasses = game_frame(case, fx, n)
    famcount = collections.Counter(d['family'] for d in gdraws)
    print('selftest: %s frame %d: %d game draws %s; passes %s' % (os.path.basename(case_path), n, len(gdraws), dict(famcount), dict(gpasses)))

    def ours_from_game(mut=None):
        res = []
        for d in gdraws:
            if d['family'] not in FAMILIES:
                continue
            o = dict(d, slots={str(k): ('map' if not v.startswith('target') and v != 'none' else 'map') for k, v in d['slots'].items()}, vals={k: list(v) for k, v in d['vals'].items()})
            res.append(o)
        if mut:
            res = mut(res)
        return res

    full = {k: (True, 'planted') for _, _, k in GAME_PASSES}
    fails = []

    def check(name, odraws, opass, token, want):
        res = diff(gdraws, gpasses, odraws, opass, fx)
        text = '\n'.join(m['what'] for m in res['missing']) + '\n' + '\n'.join(m['what'] for m in res['values'])
        got = token in text if token else bool(res['missing'] or res['values'])
        ok = got == want
        print('%s  %-34s %s' % ('PASS' if ok else 'FAIL', name, ('flagged' if got else 'clean') + ('' if token is None else ' (token "%s")' % token)))
        if not ok:
            fails.append(name)
            print('      ' + text.replace('\n', '\n      ')[:700])

    check('perfect copy reports nothing', ours_from_game(), full, None, False)
    # passes the game ran: drop them one at a time
    for feat, techs, key in GAME_PASSES:
        if sum(gpasses.get(t, 0) for t in techs):
            p = dict(full)
            p[key] = (False, 'planted')
            check('pass missing: ' + feat, ours_from_game(), p, 'pass "%s"' % feat, True)

    def no_lit(r):
        for d in r:
            if d['family'] == 'crowd':
                d['vs'] = (d['vs'] or '').replace('_lit_', '_')
        return r
    if famcount.get('crowd') and any('_lit_' in (d['vs'] or '') for d in gdraws if d['family'] == 'crowd'):
        check('crowd unlit (FX_LIT missing)', ours_from_game(no_lit), full, 'FX_LIT', True)

    def rim_black(r):
        for d in r:
            if d['family'] == 'wrestler' and '15' in d['slots']:
                d['slots']['15'] = 'black'
        return r
    if any(15 in d['slots'] for d in gdraws if d['family'] == 'wrestler'):
        check('wrestler rim slot 15 black', ours_from_game(rim_black), full, 'slot 15', True)

    def key_light(r):
        for d in r:
            if d['family'] == 'wrestler' and 'DifLghCol' in d['vals'] and len(d['vals']['DifLghCol']) >= 20:
                d['vals']['DifLghCol'][16:19] = [2.17, 2.17, 2.17]
        return r
    if any('DifLghCol' in d['vals'] and len(d['vals']['DifLghCol']) >= 20 for d in gdraws if d['family'] == 'wrestler'):
        check('wrestler key light [4] 2.17', ours_from_game(key_light), full, 'DifLghCol[4]', True)

    def alpha_spec_off(r):
        for d in r:
            if d['ps'] and 'alpha_spec' in d['ps']:
                d['ps'] = d['ps'].replace('alpha_spec', 'specular')
        return r
    if any(d['ps'] and 'alpha_spec' in d['ps'] for d in gdraws):
        check('alpha_spec -> cut-out variant', ours_from_game(alpha_spec_off), full, 'FX_ALPHA_SPEC', True)
    print('selftest: %s' % ('FAIL: ' + ', '.join(fails) if fails else 'PASS'))
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('case', nargs='?')
    ap.add_argument('--frames', default='')
    ap.add_argument('--out', default='')
    ap.add_argument('--env', action='append', default=[], metavar='KEY=VAL', help='a plant for our side (WWE_RDR_PLANT_UNLIT_CROWD=1, WWE_RDR_P1_PLANT=old_alpha, ...)')
    ap.add_argument('--json', default='')
    ap.add_argument('--max-values', type=int, default=12)
    ap.add_argument('--rig', default='')
    ap.add_argument('--look-from-stream', action='store_true', help='an entrance capture (folder): inject the stage look rows read from the stream (frame_rig.stream_look)')
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('--no-body-scale-one', action='store_true', help='S103 L14: do not tell the rig body scale 1 (the wrestlers are then holes, as in a live frame with no scale)')
    a = ap.parse_args()
    global MAX_VALUES, BODY_SCALE_ONE
    MAX_VALUES = a.max_values
    BODY_SCALE_ONE = not a.no_body_scale_one
    if a.selftest:
        return selftest()
    if not a.case:
        ap.error('a case zip is required')
    fx = Fx()
    case = Case(a.case)
    frames = [int(x) for x in a.frames.split(',')] if a.frames else sorted(case.scene)[len(case.scene) // 2:][:1]
    out = a.out or os.path.join(ROOT, 'out', 's103', 'T2', os.path.splitext(os.path.basename(a.case))[0])
    envs = dict(kv.split('=', 1) for kv in a.env)
    results = run_frames(case, fx, frames, out, envs, rig=a.rig or None, look_from_stream=a.look_from_stream)
    for n, res, gs, os_, *_ in results:
        print(render(n, res, gs, os_))
    if len(results) > 1:
        print('\nCASE SUMMARY (%d frames; worst first):' % len(results))
        for e in summary(results)[:40]:
            print('  %-7s frames %s  %s' % (e.get('kind', 'VALUE'), ','.join(map(str, e['frames'])), e['what']))
    if a.json:
        json.dump([dict(frame=n, **{k: v for k, v in res.items()}) for n, res, *_ in results], open(a.json, 'w'), indent=1, default=str)
    return 0


if __name__ == '__main__':
    sys.exit(main())
