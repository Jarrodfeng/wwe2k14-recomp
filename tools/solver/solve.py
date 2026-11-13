r"""The constant solver (Session 101, lane 16; RENDERER_ARC.md section 4 tool 6, step B3; docs\renderer\CONSTANTS.md).

For every effect constant the captured draws used: where does its value come from?
  FILE      equal, in every joined draw, to the named parameter of the mesh in the model file (the drawables atlas)
  SCENE     a camera / light / light-set / time quantity (declared by name below; the frame-uniformity of its values is reported as evidence)
  RUNTIME   anything else: game logic wrote it (the exact list is the hook list Phase C needs, arc P2)
  UNJOINED  seen, a file source is declared, but no draw could be joined to its mesh (no evidence either way)
  UNSEEN    in the effect's description, never seen in any captured draw

  py -I tools\solver\solve.py [--zips GLOB] [--frames-per-zip K] [--out DIR] [--doc PAGE.md] [--json] [--plant EFFECT:FIELD] [--strip-names]

0 game runs: reads case zips in place (game data: nothing from them is written except to out\s101\solver\, local only) and the atlas.
--plant: perturbs the first matching FILE comparison of that (effect, field) by one draw (the planted fault; the verdict must become RUNTIME).
--strip-names: drops the draw-origin section (the planted fault for the join; every draw must come out unjoined, loudly).
"""
import argparse
import collections
import glob
import json
import os
import pickle
import random
import re
import subprocess
import sys
import time
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import capread  # noqa: E402

REPO = HERE.parents[1]


def _main_checkout():
    if os.environ.get('RDR_MAIN_CHECKOUT'):
        return Path(os.environ['RDR_MAIN_CHECKOUT'])
    if (REPO / 'out' / 's101').exists():
        return REPO
    common = subprocess.run(['git', '-C', str(REPO), 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True,
                            text=True).stdout.strip()
    return Path(common).parent if common else REPO


MAIN = _main_checkout()
FX_DIR = REPO / 'src' / 'renderer' / 'shaders' / 'fx'
ATLAS = MAIN / 'out' / 'atlas' / 'atlas.json'
OUT = MAIN / 'out' / 's101' / 'solver'
KLASS = {0: 'unnamed', 1: 'model_part', 2: 'crowd', 3: 'sprite', 4: 'text', 5: 'movie', 6: 'particle', 7: 'shadow', 8: 'post', 9: 'clear',
         10: 'other'}
MODEL_PART = 1

# Camera / light / light-set / time / fog quantities, by the constants' own names. Declared, not inferred; the report shows for each one how many
# distinct values it took per frame (evidence), and a SCENE constant that varies a lot says so.
SCENE_NAMES = {
    'ViewProj', 'EyePos', 'View', 'CameraViewProj', 'ViewProjLightDepth', 'ViewLight', 'ViewLightDepth', 'TexProjLight', 'TexProjLightXY',
    'TexProjLightW', 'ConvScreen', 'PlaneInv', 'KeyLightColor', 'KeyLightVec', 'DifLghCol', 'DifLghVec', 'DifLghMax', 'DifLghMaxA', 'SkyDir',
    'SkyCol', 'GroundCol', 'LightEye', 'FirstLightVec', 'LightMin', 'ShadowPrm', 'ShadowPrm0', 'ShadowPrm1', 'ShadowPrm2', 'Time', 'FogColor',
    'FogDistance', 'LightMapParam', 'RimLightDepth', 'RimLightParam', 'RimLightParam3',
}


# ---- effects ----------------------------------------------------------------------------------------------------------------------------

class Effects:
    """hash -> effects / variants, and the constants (field, register, count) of every variant, from the .fx.json files."""

    def __init__(self):
        self.by_hash = {'vertex': collections.defaultdict(list), 'fragment': collections.defaultdict(list)}
        self.consts = {}   # (effect, stage, variant) -> [(field, register, count)]
        self.fields = collections.defaultdict(set)  # effect -> {(stage, field)}
        files = sorted(FX_DIR.glob('*.fx.json'))
        if not files:
            raise SystemExit(f'FAIL  no .fx.json files in {FX_DIR} (nothing to solve against)')
        for p in files:
            d = json.loads(p.read_text(encoding='utf-8'))
            eff = d['effect']
            for stage in ('vertex', 'fragment'):
                for v in d.get(stage, []):
                    cl = [(c['field'], c['register'], c['count']) for c in v.get('constants', [])]
                    self.consts[(eff, stage, v['name'])] = cl
                    for c in cl:
                        self.fields[eff].add((stage, c[0]))
                    for h in v.get('hashes', []):
                        self.by_hash[stage][int(h, 16)].append((eff, v['name']))

    def resolve(self, vs_hash, ps_hash, hint=None):
        """-> (effect label, {stage: constants list}, reason or None)."""
        ve = {e for e, _ in self.by_hash['vertex'].get(vs_hash, [])}
        pe = {e for e, _ in self.by_hash['fragment'].get(ps_hash, [])} if ps_hash else set()
        if not ve and not pe:
            return None, {}, 'shader hash not in any effect description'
        eff = ve & pe if (ve and pe and ve & pe) else (ve | pe)
        if len(eff) > 1 and hint in eff:
            eff = {hint}
        label = '|'.join(sorted(eff))
        out = {}
        for stage, h in (('vertex', vs_hash), ('fragment', ps_hash)):
            lists = []
            for e in eff:
                for ee, vn in self.by_hash[stage].get(h, []):
                    if ee == e:
                        lists.append(self.consts[(ee, stage, vn)])
                        break
            if lists:
                out[stage] = lists[0] if len(lists) == 1 else [c for c in lists[0] if all(c in l for l in lists[1:])]
        return label, out, None


# ---- the atlas --------------------------------------------------------------------------------------------------------------------------

class Atlas:
    """Meshes of the files, as the solver needs them: (effect, vertices, indices, non-sampler parameter values) per (model, mesh)."""

    def __init__(self):
        if not ATLAS.exists():
            raise SystemExit(f'FAIL  {ATLAS} is missing (run tools\\atlas first): the file side of the comparison cannot be read')
        cache = OUT / 'atlas_index.pkl'
        st = ATLAS.stat()
        key = (st.st_size, int(st.st_mtime))
        data = None
        if cache.exists():
            try:
                k, data = pickle.loads(cache.read_bytes())
                if k != key:
                    data = None
            except Exception:
                data = None
        if data is None:
            data = []
            with open(ATLAS, encoding='utf-8') as f:
                for line in f:
                    line = line.strip().rstrip(',')
                    if '"mesh"' not in line[:30]:
                        continue
                    r = json.loads(line)
                    if not r.get('loaded'):
                        continue
                    params = {p['n']: p['v'] for p in r.get('params', []) if 'v' in p}
                    data.append((r['pac'], r['entry'], r['mesh'], r['effect'], r['vertices'], r['index_count'], params))
            OUT.mkdir(parents=True, exist_ok=True)
            cache.write_bytes(pickle.dumps((key, data), protocol=4))
        if not data:
            raise SystemExit(f'FAIL  {ATLAS} holds no loaded meshes')
        self.bysig = collections.defaultdict(set)   # (vertices, indices) -> {model}
        self.models = collections.defaultdict(dict)  # model -> {mesh index -> (effect, v, i, params)}
        for pac, ent, mi, eff, v, i, params in data:
            m = (pac, ent)
            self.bysig[(v, i)].add(m)
            self.models[m][mi] = (eff, v, i, params)
        self.effects = {t[0] for t in self.models.values() for t in t.values()}
        self.pnames = collections.defaultdict(set)   # effect -> every parameter name its meshes carry
        for m in self.models.values():
            for eff, _, _, params in m.values():
                self.pnames[eff].update(params)
        self.mesh_count = len(data)


# ---- helpers ----------------------------------------------------------------------------------------------------------------------------

def param_value(params, spec):
    m = re.fullmatch(r'(\w+?)(?:\[(\d)\])?', spec)
    name, idx = m.group(1), int(m.group(2) or 0)
    v = params.get(name)
    if v is None or idx >= len(v):
        return None
    return v[idx]


def close(a, b):
    return abs(a - b) <= 1e-6 + 1e-5 * max(abs(a), abs(b))


def load_images(src):
    """images.bin -> {id: (w, h)}; only for reports (not needed by the solver's verdicts)."""
    return {}


def scene_label(zpath, manifest):
    folder = Path(zpath).parent.name
    tag = re.sub(r'^\d+_\d+_', '', folder)
    tag = re.sub(r'^s100_owner_', '', tag)
    extra = ''
    try:   # the run's own settings file name says the resolution arm (the manifest's render size is 0 x 0 in these zips)
        plan = json.loads((Path(zpath).parent / 'plan.json').read_text(encoding='utf-8'))
        for f in plan.get('flags', []):
            m = re.search(r'settings_file=\S*?([^\\/\s]+?)\.json', f)
            if m:
                extra = ' ' + m.group(1).replace('settings_', '')
    except Exception:
        pass
    ts = re.match(r'(\d{8})_(\d{6})', folder)
    return f'{tag}{extra} {ts.group(2)[:4]}' if ts else tag + extra, Path(zpath).stem.split('_')[-1]


# ---- the run ----------------------------------------------------------------------------------------------------------------------------

class Stat:
    __slots__ = ('draws', 'checked', 'equal', 'scenes', 'distinct', 'lo', 'hi', 'mism_scenes', 'mapped', 'ambiguous', 'absent', 'alldistinct',
                 'samples', 'nsamp', 'unj', 'disputed', 'disputed_eq')

    def __init__(self):
        self.draws = self.checked = self.equal = self.ambiguous = self.absent = 0
        self.alldistinct = set()   # distinct values over all draws, capped
        self.samples = []          # (values, params) of joined draws, for the mapping hints (reservoir of 40)
        self.nsamp = 0
        self.unj = {}              # component -> distinct values, for components no file parameter is declared for (capped)
        self.disputed = self.disputed_eq = 0   # compared in a draw whose join was rejected (most constants disagreed): count, and how many agreed
        self.scenes = set()
        self.mism_scenes = set()
        self.distinct = []
        self.lo = self.hi = None
        self.mapped = False


def pick_frames(n, k):
    if n <= k:
        return list(range(n))
    if k <= 1:
        return [0]
    idx = sorted({int(round(i * (n - 1) / (k - 1))) for i in range(k)})
    return idx


def run(zips, fx, atlas, fieldmap, frames_per_zip, plant=None, strip_names=False, log=print):
    stats = collections.defaultdict(Stat)   # (effect, stage, field) -> Stat
    spec_cache = {}

    def spec_for(field, base):
        """The declared source of a constant (fieldmap.json), else the parameter of the same name in the effect's meshes (g_f4NAME, g_fNAME ...)."""
        k = (field, base)
        if k not in spec_cache:
            s = fieldmap.get(field)
            if s is None:
                names = atlas.pnames.get(base, ())
                for pre in ('g_f4', 'g_f3', 'g_f2', 'g_f', 'g_i', 'g_b'):
                    if pre + field in names:
                        s = [f'{pre}{field}[{i}]' for i in range(4)]
                        break
            spec_cache[k] = s
        return spec_cache[k]

    rng = random.Random(1)

    def mesh_effect(name):
        """The atlas effect name a (possibly LM / typo'd) effect-description name stands for, else None."""
        for n in (name, name[:-2] if name.endswith('LM') else None, name[:-2] + 'l' if name.endswith('LM') else None):
            if n and n in atlas.effects:
                return n
        return None

    scenes = []                             # per zip: counts
    planted = {'done': False, 'key': plant, 'stage': None}
    for zpath in zips:
        src = capread.Source(zpath)
        label, trig = scene_label(zpath, src.manifest)
        sc = {'scene': label, 'trigger': trig, 'zip': Path(zpath).name, 'frames_total': len(src.frames), 'frames_read': 0, 'draws': 0,
              'joined': 0, 'joined_ambiguous': 0, 'unjoined': collections.Counter(), 'no_file_source': collections.Counter()}
        for fi in pick_frames(len(src.frames), frames_per_zip):
            fr = capread.Frame(src.read(src.frames[fi]))
            sc['frames_read'] += 1
            origins = None if (strip_names or not fr.has_origins) else fr.origins
            # object -> candidate models (every part must fit its (vertices, indices) at the part's index)
            objs = collections.defaultdict(lambda: collections.defaultdict(set))
            if origins is not None:
                for i, d in enumerate(fr.draws):
                    o = origins[i]
                    if o['klass'] == MODEL_PART and o['object']:
                        objs[int(o['object'])][int(o['part'])].add((int(d['vertex_count']), int(d['index_count'])))
            objmodels = {}
            for ob, parts in objs.items():
                c = None
                for p, sigs in parts.items():
                    for sg in sigs:
                        s = atlas.bysig.get(sg, set())
                        c = set(s) if c is None else c & s
                if c:
                    c = {m for m in c if all(any((atlas.models[m].get(p) or (0, 0, 0))[1:3] == sg for sg in sigs) for p, sigs in parts.items())}
                objmodels[ob] = c or set()
            seen_frame = collections.defaultdict(set)
            for i, d in enumerate(fr.draws):
                sc['draws'] += 1
                o = origins[i] if origins is not None else None
                klass = int(o['klass']) if o is not None else 0
                cands = []
                reason = None
                eff_hint = None
                # mesh candidates first (they hint the effect for hashes shared by several effects)
                eff_label, cons, why = fx.resolve(int(d['vs_hash']), int(d['ps_hash']))
                if why:
                    sc['unjoined'][why] += 1
                    continue
                if '|' in eff_label and o is not None and klass == MODEL_PART and int(o['object']):
                    # the hashes are shared by several effects: the mesh the object's parts fit says which one this draw is
                    part_, sg_ = int(o['part']), (int(d['vertex_count']), int(d['index_count']))
                    fit = {mesh_effect(e): e for e in eff_label.split('|')}
                    have = {atlas.models[m][part_][0] for m in objmodels.get(int(o['object']), set())
                            if part_ in atlas.models[m] and atlas.models[m][part_][1:3] == sg_}
                    pick = [e for me, e in fit.items() if me in have]
                    if len(pick) == 1:
                        eff_label, cons, why = fx.resolve(int(d['vs_hash']), int(d['ps_hash']), pick[0])
                base = mesh_effect(eff_label) or eff_label
                if o is None:
                    sc['unjoined']['unnamed: the capture has no draw origin names' if not strip_names else
                                   'unnamed: names stripped (planted fault)'] += 1
                    reason = 'unnamed'
                elif klass == 0:
                    sc['unjoined']['unnamed: draw origin class 0'] += 1
                    reason = 'unnamed'
                elif klass != MODEL_PART:
                    sc['no_file_source'][f'class {KLASS.get(klass, klass)}'] += 1
                    reason = 'nofile'
                elif base not in atlas.effects:
                    sc['no_file_source']['model part drawn by a pass effect (no mesh material)'] += 1
                    reason = 'nofile'
                else:
                    ob, part, sg = int(o['object']), int(o['part']), (int(d['vertex_count']), int(d['index_count']))
                    ms = objmodels.get(ob, set())
                    if not ob:
                        sc['unjoined']['no object address'] += 1
                        reason = 'unjoined'
                    elif not ms:
                        sc['unjoined']['no model in the files has this object\'s parts (vertex / index counts)'] += 1
                        reason = 'unjoined'
                    else:
                        cands = [atlas.models[m][part] for m in ms if part in atlas.models[m] and atlas.models[m][part][1:3] == sg
                                 and atlas.models[m][part][0] == base]
                        if not cands:
                            sc['unjoined']['no mesh of this effect fits the draw (effect / counts)'] += 1
                            reason = 'unjoined'
                        else:
                            vals = {json.dumps(c[3], sort_keys=True) for c in cands}
                            # several meshes can fit (same counts in other models): the constants they agree on are still compared, the rest are
                            # counted per constant as ambiguous; a draw is "joined" when every fitting mesh has the same parameters
                            sc['joined' if len(vals) == 1 else 'joined_ambiguous'] += 1
                            reason = 'joined'
                plist = [c[3] for c in cands] if reason == 'joined' else None
                # constants: first compare against the file (nothing is counted yet), then decide whether the join is believable
                pending = []   # (stat, kind, payload)
                cmp_ = []      # (stat, equal)
                for stage, bank_col in (('vertex', 'vs_bank'), ('fragment', 'ps_bank')):
                    if stage == 'fragment' and not int(d['ps_hash']):
                        continue
                    bank = int(d[bank_col])
                    if bank >= len(fr.banks):
                        continue
                    row = fr.banks[bank]
                    for field, reg, cnt in cons.get(stage, []):
                        key = (eff_label, stage, field)
                        st = stats[key]
                        vals = row[4 * reg:4 * (reg + cnt)].copy()
                        st.draws += 1
                        st.scenes.add(label)
                        seen_frame[key].add(vals.tobytes())
                        if cnt * 4 <= 16:
                            if st.lo is None:
                                st.lo, st.hi = vals.copy(), vals.copy()
                            else:
                                n = min(len(st.lo), len(vals))
                                st.lo, st.hi = np.fmin(st.lo[:n], vals[:n]), np.fmax(st.hi[:n], vals[:n])
                        if len(st.alldistinct) < 65:
                            st.alldistinct.add(vals.tobytes())
                        spec = spec_for(field, base)
                        if spec is not None and base in atlas.effects:
                            st.mapped = True
                        if plist is not None and len({json.dumps(p, sort_keys=True) for p in plist}) == 1:
                            pending.append((st, 'sample', (vals.copy(), plist[0])))   # every unambiguously joined draw feeds the mapping hints
                        if spec is None or plist is None:
                            continue
                        per = []
                        for params in plist:
                            cs = [(k, param_value(params, s)) for k, s in enumerate(spec[:cnt * 4]) if s is not None]
                            per.append(tuple((k, v) for k, v in cs if v is not None))
                        if len(set(per)) > 1:
                            pending.append((st, 'ambiguous', None))   # the fitting meshes disagree on this constant: no verdict from this draw
                            continue
                        comps = per[0]
                        if not comps:
                            pending.append((st, 'absent', None))      # joined, but the mesh in the file carries no such parameter
                            continue
                        if (plant and not planted['done'] and plant[:2] == (eff_label, field) and (len(plant) < 3 or plant[2] == stage)
                                and all(close(float(vals[k]), v) for k, v in comps)):
                            vals[comps[0][0]] += 0.5   # the planted fault: one FILE constant perturbed in one draw
                            planted['done'], planted['stage'] = True, stage
                        eq = all(close(float(vals[k]), v) for k, v in comps)
                        cmp_.append((st, eq))
                        pending.append((st, 'cmp', (eq, vals, {k for k, _ in comps})))
                suspect = len(cmp_) >= 3 and 2 * sum(1 for _, e in cmp_ if not e) >= len(cmp_)
                if suspect:
                    # most of this draw's file-mapped constants disagree with the mesh we joined: more likely the wrong mesh than a draw that
                    # overrides everything. It gives no evidence either way and is counted as unjoined.
                    sc['joined' if len({json.dumps(p, sort_keys=True) for p in plist}) == 1 else 'joined_ambiguous'] -= 1
                    sc['unjoined']['join not confirmed: most of the draw\'s file-mapped constants disagree with the mesh'] += 1
                    for st, eq in cmp_:   # not discarded silently: each constant compared here is counted as disputed on its own verdict
                        st.disputed += 1
                        st.disputed_eq += bool(eq)
                    continue
                for st, kind, payload in pending:
                    if kind == 'ambiguous':
                        st.ambiguous += 1
                    elif kind == 'absent':
                        st.absent += 1
                    elif kind == 'sample':
                        st.nsamp += 1
                        if len(st.samples) < 40:
                            st.samples.append(payload)
                        else:
                            j = rng.randrange(st.nsamp)
                            if j < 40:
                                st.samples[j] = payload
                    else:
                        eq, vals, judged = payload
                        st.checked += 1
                        if eq:
                            st.equal += 1
                        else:
                            st.mism_scenes.add(label)
                        for k in range(len(vals)):   # components no file parameter is declared for (null in the field map, or beyond it)
                            if k not in judged:
                                u = st.unj.setdefault(k, set())
                                if len(u) < 16:
                                    u.add(float(vals[k]))
            for key, s in seen_frame.items():
                stats[key].distinct.append(len(s))
        sc['unjoined'] = dict(sc['unjoined'])
        sc['no_file_source'] = dict(sc['no_file_source'])
        scenes.append(sc)
        log(f"  {label:28} {len(src.frames):5} frames ({sc['frames_read']} read)  draws {sc['draws']:6}  joined {sc['joined']:5}  "
            f"unjoined {sum(sc['unjoined'].values()):5}  no file source {sum(sc['no_file_source'].values()):5}")
    return stats, scenes, planted


def hints(st):
    """Component k of a constant equals parameter p[i] in >= 90% of up to 40 joined draws, with a value that is not 0 / 1 / -1 in all of them."""
    out = []
    if len(st.samples) < 5:
        return out
    n = len(st.samples)
    for k in range(min(len(st.samples[0][0]), 16)):
        for pname in st.samples[0][1]:
            for i in range(4):
                ok = trivial = 0
                for vals, params in st.samples:
                    pv = param_value(params, f'{pname}[{i}]')
                    if pv is not None and k < len(vals) and close(float(vals[k]), pv):
                        ok += 1
                        trivial += pv in (0.0, 1.0, -1.0)
                if ok >= 0.9 * n and trivial < ok:
                    out.append(f'{k}={pname}[{i}]')
    return out[:8]


def comp_name(k):
    return 'xyzw'[k % 4] if k < 4 else f'r{k // 4}.' + 'xyzw'[k % 4]


def classify(stats, fx, atlas, fieldmap):
    out = {}   # effect -> field key -> record
    names = collections.defaultdict(dict)
    for (eff, stage, field), st in stats.items():
        if st.draws == 0:
            continue
        rec = {'stage': stage, 'draws': st.draws, 'scenes': sorted(st.scenes), 'checked': st.checked, 'equal': st.equal, 'ambiguous': st.ambiguous}
        dm = sorted(st.distinct)
        rec['values_per_frame'] = {'min': dm[0], 'median': dm[len(dm) // 2], 'max': dm[-1]} if dm else None
        partial = {k: len(v) for k, v in sorted(st.unj.items()) if len(v) > 1}   # unjudged components that take several values
        if st.mapped and st.checked:
            if st.equal == st.checked and not partial:
                rec['verdict'], rec['why'] = 'FILE', f'equal to the file in all {st.checked} joined draws'
            elif st.equal == st.checked:
                rec['verdict'] = 'RUNTIME'
                rec['why'] = (f'partial: the file-named components equal the file in all {st.checked} joined draws, but component(s) '
                              + ', '.join(f'{comp_name(k)} ({n} values)' for k, n in partial.items()) + ' have no file source and move')
                rec['partial'] = [comp_name(k) for k in partial]
            else:
                rec['verdict'] = 'RUNTIME'
                rec['why'] = f'differs from the file value in {st.checked - st.equal} of {st.checked} joined draws (scenes: {", ".join(sorted(st.mism_scenes))})'
        elif st.mapped and st.absent:
            rec['verdict'], rec['why'] = 'RUNTIME', f'the joined meshes carry no such parameter ({st.absent} draws): the game fills it'
        elif st.mapped:
            rec['verdict'], rec['why'] = 'UNJOINED', 'a file source is declared but no draw using it was joined to its mesh (or the meshes disagreed)'
        elif field in SCENE_NAMES:
            rec['verdict'], rec['why'] = 'SCENE', 'declared by name (camera / light / time quantity), NOT tested against a quantity the capture carries'
        else:
            rec['verdict'], rec['why'] = 'RUNTIME', 'no file parameter declared for it, not a scene quantity'
        judged_unj = sorted(comp_name(k) for k, v in st.unj.items() if len(v) == 1)
        if st.mapped and st.checked:
            rec['unjudged'] = {'one_value': judged_unj, 'moving': [comp_name(k) for k in partial]}   # components with no file source
        if st.disputed:
            rec['disputed'] = {'draws': st.disputed, 'agreeing_with_file': st.disputed_eq}   # compared in draws whose join was rejected
            rec['why'] += f'; disputed: {st.disputed} further draws had their join rejected ({st.disputed_eq} of them agreed with the file)'
        rec['values_total'] = len(st.alldistinct) if len(st.alldistinct) < 65 else '64+'
        if rec['verdict'] in ('RUNTIME', 'SCENE'):
            if rec['verdict'] == 'RUNTIME':
                rec['varies'] = len(st.alldistinct) > 1   # False: one value in every captured draw (a constant default, or the corpus is too small)
            h = hints(st)
            if h:
                rec['value_hints'] = h   # file parameters whose values equal this constant's components in >= 90% of the joined draws (mapping gaps)
        if st.lo is not None and rec['verdict'] == 'RUNTIME':
            rec['range'] = {'lo': [round(float(x), 5) for x in st.lo], 'hi': [round(float(x), 5) for x in st.hi]}   # LOCAL ONLY (game values)
        names[eff][f'{stage}:{field}'] = rec
    # unseen: described, never drawn
    for eff, fl in sorted(fx.fields.items()):
        for stage, field in sorted(fl):
            if (eff, stage, field) not in stats:
                names[eff][f'{stage}:{field}'] = {'stage': stage, 'verdict': 'UNSEEN', 'draws': 0, 'why': 'in the description, in no captured draw'}
    return {k: dict(sorted(v.items())) for k, v in sorted(names.items())}


def summarize(eff_rec):
    """effect -> counts per verdict."""
    out = {}
    for eff, recs in eff_rec.items():
        c = collections.Counter(r['verdict'] for r in recs.values())
        out[eff] = {v: c.get(v, 0) for v in ('FILE', 'SCENE', 'RUNTIME', 'UNJOINED', 'UNSEEN')}
    return out


def runtime_list(eff_rec):
    return {eff: sorted(k.split(':', 1)[1] + ('/' + r['stage'][0]) for k, r in recs.items() if r['verdict'] == 'RUNTIME')
            for eff, recs in eff_rec.items() if any(r['verdict'] == 'RUNTIME' for r in recs.values())}


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--zips', default=str(MAIN / 'out' / 's101' / 'runs' / '*' / 'case_*.zip'))
    ap.add_argument('--frames-per-zip', type=int, default=10)
    ap.add_argument('--out', default=str(OUT))
    ap.add_argument('--doc', default='')
    ap.add_argument('--plant', default='')
    ap.add_argument('--strip-names', action='store_true')
    ap.add_argument('--quiet', action='store_true')
    a = ap.parse_args(argv)
    t0 = time.time()
    zips = sorted(glob.glob(a.zips))
    if not zips:
        print(f'FAIL  no case zips match {a.zips} (a solver that reads nothing must fail)')
        return 1
    fx = Effects()
    atlas = Atlas()
    fieldmap = json.loads((HERE / 'fieldmap.json').read_text(encoding='utf-8'))['fields']
    plant = tuple(a.plant.split(':')) if a.plant else None   # EFFECT:FIELD[:vertex|fragment]
    log = (lambda *x: None) if a.quiet else print
    log(f'constant solver: {len(zips)} zips, {atlas.mesh_count} atlas meshes, {len(fx.consts)} effect variants')
    stats, scenes, planted = run(zips, fx, atlas, fieldmap, a.frames_per_zip, plant, a.strip_names, log)
    if plant and not planted['done']:
        print(f'FAIL  planted fault {a.plant}: no FILE comparison of that constant was made, nothing was perturbed')
        return 1
    eff_rec = classify(stats, fx, atlas, fieldmap)
    summ = summarize(eff_rec)
    total_draws = sum(s['draws'] for s in scenes)
    total_joined = sum(s['joined'] for s in scenes)
    if total_draws == 0:
        print('FAIL  0 draws read')
        return 1
    result = {'seconds': round(time.time() - t0, 1), 'zips': [Path(z).name for z in zips], 'frames_per_zip': a.frames_per_zip, 'scenes': scenes,
              'summary': summ, 'runtime_list': runtime_list(eff_rec), 'effects': eff_rec, 'plant': a.plant, 'strip_names': a.strip_names}
    outdir = Path(a.out)
    outdir.mkdir(parents=True, exist_ok=True)
    if not a.plant and not a.strip_names:
        (outdir / 'constants.json').write_text(json.dumps(result, indent=1), encoding='utf-8')
    # per scene table + per effect table
    log('')
    log(f'{"scene":32} {"draws":>7} {"joined":>7} {"j.ambig":>8} {"unjoined":>9} {"nofile":>7}  unjoined by reason')
    for s in scenes:
        log(f'{s["scene"]:32} {s["draws"]:7} {s["joined"]:7} {s["joined_ambiguous"]:8} {sum(s["unjoined"].values()):9} '
            f'{sum(s["no_file_source"].values()):7}  ' + '; '.join(f'{k}: {v}' for k, v in s['unjoined'].items()))
    log('')
    log(f'{"effect":22} {"FILE":>5} {"SCENE":>6} {"RUNTIME":>8} {"UNJOINED":>9} {"unseen":>7}')
    for eff, c in summ.items():
        if c['FILE'] or c['SCENE'] or c['RUNTIME'] or c['UNJOINED']:
            log(f'{eff:22} {c["FILE"]:5} {c["SCENE"]:6} {c["RUNTIME"]:8} {c["UNJOINED"]:9} {c["UNSEEN"]:7}')
    if a.doc:
        import doc_page
        Path(a.doc).write_text(doc_page.render(result, atlas.mesh_count), encoding='utf-8')
        log(f'wrote {a.doc}')
    log(f'done in {result["seconds"]} s')
    # verdict line for the gate
    if a.plant:
        key = plant
        v = None
        stg = planted['stage']
        r = eff_rec.get(key[0], {}).get(f'{stg}:{key[1]}')   # the stage that was perturbed, not whichever stage matched last
        v = r['verdict'] if r else None
        print(f'planted fault {a.plant} (stage {stg}): verdict {v}')
        return 0 if v == 'RUNTIME' else 1
    if not a.strip_names and total_joined + sum(s['joined_ambiguous'] for s in scenes) == 0:
        print('FAIL  nothing joined to a mesh in any scene: the join is broken or the captures carry no model parts')
        return 1
    if a.strip_names:
        joined = sum(s['joined'] for s in scenes)
        print(f'names stripped: {total_draws} draws, {joined} joined, {sum(sum(s["unjoined"].values()) for s in scenes)} unjoined (all must be unjoined)')
        return 0 if joined == 0 else 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
