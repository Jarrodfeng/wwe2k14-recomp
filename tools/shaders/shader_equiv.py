#!/usr/bin/env python3
r"""S103 T6: the SHADER EQUIVALENCE TEST. Every effect variant we hand-wrote (src\renderer\shaders\fx\*.fx.json) against the game's own microcode, offline,
0 game runs, no capture, no scene (S103_FABLE_REVIEW.md 2.4).

How: this is the driver over tools\shaders\harness\check_effect.py (S100), which does the actual comparison: the game's shader (microcode read from the
player's own pacs -> the SDK translator -> SPIR-V -> compute) and ours (the variant's GLSL with its defines) run on the same random inputs (constants by
the JSON's field -> register map, vertex attributes, interpolators, a fake sampler that returns the same chosen texel values to both) and every output
(position, interpolators, colour outputs, kill, depth) is compared per channel (tolerance 1e-3 of the output's scale). The driver adds: a coverage report
over the whole inventory (db\shaders\inventory.json), a fixed sample (--sample), and PLANTED TWINS (--twins): copies of our shaders with a known past-bug
shape planted (the page gain applied twice, a light vector's sign flipped, a dropped rim term), each of which must be caught by the variant it touches
(and the untouched control variants must stay equal). Everything the game gave stays under out\ (rule 1); nothing is committed from it.

Calls (run from any checkout; out\ and game_data\ are found in the main checkout):
  py tools\shaders\shader_equiv.py --sample                 the fixed sample of variants (the gate lane s103_T6)
  py tools\shaders\shader_equiv.py --twins                  the planted twins + their controls (exit 0 only if all caught and all controls equal)
  py tools\shaders\shader_equiv.py --effects yAudience,yCh_skin [--variant REGEX] [--stage vertex|fragment]
  py tools\shaders\shader_equiv.py --probes                 harness-limited effects with the one limited line probed (yGlareEtc)
  py tools\shaders\shader_equiv.py --all                  every effect with a .fx.json (~70 min; whole-effect jobs in parallel)
  options: --jobs N (default 6) --rounds N (thousands x 4096 inputs per hash, default 1) --seed N --report FILE
Needs once: the translator exe (pwsh tools\shaders\harness\check_effect_build_translate.ps1), Vulkan SDK, the disassembler (check_effect.py says).
Exit: 0 = every tested variant equal (--twins: every twin caught, every control equal); 1 otherwise.
"""
import argparse, concurrent.futures as cf, glob, json, os, re, shutil, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(HERE, 'harness'))
import dataroot
DATA = dataroot.data_root(REPO)
FXDIR = os.path.join(REPO, 'src', 'renderer', 'shaders', 'fx')
CHECK = os.path.join(HERE, 'harness', 'check_effect.py')
OUT = os.path.join(DATA, 'out', 's103', 'shader_equiv')
INVENTORY = os.path.join(REPO, 'db', 'shaders', 'inventory.json')

# (effect, stage, variant name): the classes that matter now (S103 T6 brief): BG default / light map, prop, audience (lit / lm), skin (base / shadow),
# particle, 2D (UI quad / glyph). Names, not hashes: a renamed variant fails loudly.
SAMPLE = [
    ('yBG_Default', 'fragment', 'base'), ('yBG_Default', 'fragment', 'shadow'), ('yBG_Default', 'fragment', 'specular'),
    ('yBG_Default', 'vertex', 'base'), ('yBG_Default', 'vertex', 'vertex_lm'), ('yBG_Default', 'vertex', 'shadow_vertex_lm'),
    ('yBG_DefaultLM', 'fragment', 'base'), ('yBG_DefaultLM', 'fragment', 'shadow'), ('yBG_DefaultLM', 'vertex', 'base'),
    ('yProp_Default', 'fragment', 'base'), ('yProp_Default', 'fragment', 'spec'), ('yProp_Default', 'vertex', 'base'),
    ('yProp_DefaultLM', 'fragment', 'lightmap'), ('yProp_DefaultLM', 'fragment', 'spec_lightmap'),
    ('yAudience', 'fragment', 'base'), ('yAudience', 'fragment', 'normal_map'), ('yAudience', 'fragment', 'lm_pixel'),
    ('yAudience', 'fragment', 'normal_map_lm_pixel'), ('yAudience', 'fragment', 'normal_map_specular_lm_pixel'),
    ('yAudience', 'vertex', 'frame'), ('yAudience', 'vertex', 'lm_vertex'), ('yAudience', 'vertex', 'lit_lm_uv'),
    ('yCh_skin', 'fragment', 'base'), ('yCh_skin', 'fragment', 'shadow'), ('yCh_skin', 'fragment', 'change_color_shadow'),
    ('yCh_skin', 'fragment', 'cube_specular_shadow'), ('yCh_skin', 'vertex', 'base'), ('yCh_skin', 'vertex', 'skinned'),
    ('yParticle', 'fragment', 'base'), ('yParticle', 'vertex', None), ('ySoftParticle', 'fragment', 'base'),
    ('yRect', 'fragment', 'base'), ('yRect', 'fragment', 'gradation'), ('yRect', 'vertex', 'base'),
    ('ySprite', 'fragment', 'tinted'), ('ySprite', 'fragment', 'hsl'), ('ySprite', 'vertex', None),
]

# Planted twins: (name, effect, file, old text, new text, [(stage, variant)] that MUST differ, [(stage, variant)] controls that must stay equal, the bug shape).
TWINS = [
    ('page_gain_twice_audience', 'yAudience', 'yAudience.frag',
     'texture(textures[13], inUv.zw).rgb * fc.LightMapParam.z;', 'texture(textures[13], inUv.zw).rgb * fc.LightMapParam.z * fc.LightMapParam.z;',
     [('fragment', 'lm_pixel'), ('fragment', 'normal_map_lm_pixel')], [('fragment', 'base'), ('fragment', 'normal_map')],
     'the light-map page gain applied twice (L13b chased this as the 1.4x; it was a rig artefact, but the shape is real)'),
    ('page_gain_twice_bg_lm', 'yBG_BumpMapLM', 'fx_bg.glsl',
     'color *= texture(textures[13], inLm).xyz * fc.LightMapParam.z;', 'color *= texture(textures[13], inLm).xyz * fc.LightMapParam.z * fc.LightMapParam.z;',
     [('fragment', 'base'), ('fragment', 'shadow')], [('vertex', 'base')], 'the same shape in the background light-map pixel stage (BumpMapLM)'),
    ('light_vector_sign_audience', 'yAudience', 'yAudience.frag',
     'max(dot(n, -fc.DifLghVec[i].xyz), fc.LightMin[i])', 'max(dot(n, fc.DifLghVec[i].xyz), fc.LightMin[i])',
     [('fragment', 'normal_map'), ('fragment', 'normal_map_lm_pixel')], [('fragment', 'base'), ('fragment', 'lm_pixel')],
     'a negated light vector (the audience light points at the light)'),
    ('light_vector_sign_skin_key', 'yCh_skin', 'fx_skin.glsl',
     'max(dot(fx_normalize(n + V), fc.DifLghVec[4].xyz), 0.0)', 'max(dot(fx_normalize(n + V), -fc.DifLghVec[4].xyz), 0.0)',
     [('fragment', 'base'), ('fragment', 'shadow')], [('vertex', 'base'), ('vertex', 'skinned')], 'the key light vector with the wrong sign in the wrestler skin highlight'),
    ('rim_term_dropped_skin', 'yCh_skin', 'fx_skin.glsl',
     'rim *= (1.0 - abs(nv)) * ((gloss - 1.0) * fc.RimLightParam.y + 1.0) * fc.RimLightParam.z;', 'rim *= 1.0;',
     [('fragment', 'base'), ('fragment', 'shadow')], [('vertex', 'base'), ('vertex', 'skinned')], 'the rim term dropped from the wrestler skin (Fresnel, gloss and strength factors)'),
]

# L13 re-find: our VLM (vertex light map) and base outputs equal the game's (what L13b established by a per-draw replay).
L13_EQUALS = [('yBG_Default', 'vertex', 'vertex_lm'), ('yBG_Default', 'fragment', 'base'), ('yBG_DefaultLM', 'vertex', 'base'),
              ('yBG_DefaultLM', 'fragment', 'base'), ('yAudience', 'vertex', 'lm_vertex'), ('yAudience', 'fragment', 'lm_pixel')]

# Probes: our shader with ONE harness-limited line replaced by what the game's own microcode computes, so the rest can still be compared (reported as
# equal-with-probe, never as a plain equal). yGlareEtc (32 of 33 variants differ plain, with or without the S102 R18-B2 change; S103 T6 finding): since
# S101 lane 34 review F our FXAA tap spacing is textureSize(slot 0), the game's is the interpolator zw (0.5 / 1280, 0.5 / 720 from its vertex stage); the
# harness's fake textureSize is 1x1 and its zw random, so the two cannot agree. The probe takes the texel from zw, as the game does.
PROBES = [('glare_texel_as_game', 'yGlareEtc', 'yGlareEtc.frag',
           ['1.0 / vec2(textureSize(textures[0], 0))', 'vec2 corner = uv - 0.5 * texel;'],
           ['vec2(1.0 / 1280.0, 1.0 / 720.0)', 'vec2 corner = corner_unused;'],
           'FXAA taps as the game: the corner from the interpolator zw, the spacing the literal 1/1280, 1/720')]

LINE = re.compile(r'^(PASS|FAIL) (VS|PS) ([0-9A-F]{16}) (\S+)\s+worst (\S+)\s+(\S+)\s*(.*)$')

def load_spec(root, effect):
    return json.load(open(os.path.join(root, effect + '.fx.json')))

def resolve(root, effect, stage, name):
    """-> [(hash hex, variant name)] of the variant (name None = every variant of the stage)."""
    out = []
    for v in load_spec(root, effect).get(stage, []):
        if name is None or v['name'] == name:
            out += [(h.upper().zfill(16), v['name']) for h in v.get('hashes', [])]
    if not out:
        raise SystemExit('no variant %s/%s/%s in %s' % (effect, stage, name, root))
    return out

def run_check(root, effect, hx, rounds, seed):
    """One check_effect.py call (one hash, or the whole effect when hx is None) -> [result dicts]."""
    cmd = [sys.executable, '-I', CHECK, effect, '--root', root, '--rounds', str(rounds), '--seed', str(seed)]
    if hx:
        cmd += ['--hash', hx]
    r = subprocess.run(cmd, capture_output=True, text=True)
    res = []
    for ln in r.stdout.splitlines():
        m = LINE.match(ln)
        if m:
            ok, kind, h, var, worst, out, det = m.groups()
            res.append({'effect': effect, 'kind': kind, 'hash': h, 'variant': var, 'equal': ok == 'PASS', 'worst': None if worst == '-' else float(worst),
                        'output': out, 'detail': det.strip()})
    if not res:
        res.append({'effect': effect, 'kind': '?', 'hash': hx or '-', 'variant': '-', 'equal': False, 'worst': None, 'output': 'error',
                    'detail': (r.stdout + r.stderr)[-400:].replace('\n', ' | ')})
    return res

NOT_TESTED = ('missing', 'no microcode', 'error', 'duplicate', 'unknown hash')

def run_units(units, jobs, rounds, seed):
    """units: [(root, effect, hash or None)] -> [results]; parallel, each unit a subprocess."""
    results = []
    t0 = time.time()
    with cf.ThreadPoolExecutor(max_workers=jobs) as ex:
        futs = {ex.submit(run_check, root, eff, hx, rounds, seed): (root, eff, hx) for root, eff, hx in units}
        n = 0
        for f in cf.as_completed(futs):
            root, eff, hx = futs[f]
            n += 1
            for r in f.result():
                r['root'] = root
                results.append(r)
            if n % 20 == 0 or n == len(units):
                print('  %d / %d units, %.0f s' % (n, len(units), time.time() - t0), flush=True)
    return results

def summarise(results, title):
    tested = [r for r in results if r['output'] not in NOT_TESTED]
    eq = [r for r in tested if r['equal']]
    diff = [r for r in tested if not r['equal']]
    other = [r for r in results if r['output'] in NOT_TESTED]
    print('%s: %d hashes tested, %d equal, %d DIFFER, %d not testable' % (title, len(tested), len(eq), len(diff), len(other)))
    for r in diff:
        print('  DIFFER %s %s %s %s first diff in %s: %s' % (r['effect'], r['kind'], r['hash'], r['variant'], r['output'], r['detail'][:240]))
    for r in other:
        print('  NOT TESTED %s %s %s %s: %s %s' % (r['effect'], r['kind'], r['hash'], r['variant'], r['output'], r['detail'][:160]))
    return tested, eq, diff, other

def inventory_coverage():
    inv = json.load(open(INVENTORY))['effects']
    fx = {os.path.basename(p)[:-8] for p in glob.glob(os.path.join(FXDIR, '*.fx.json'))}
    tot = cov = groups = gcov = 0
    for e, v in inv.items():
        n = len(v['hashes'])
        tot += n
        groups += v.get('groups', 0)
        if e in fx:
            cov += n
            gcov += v.get('groups', 0)
    return {'inventory_hashes': tot, 'in_effects_with_a_fx_json': cov, 'effects_in_inventory': len(inv), 'effects_with_fx_json': len(set(inv) & fx),
            'without_fx_json': sorted(set(inv) - fx)}

def make_twin_root(name, filename, old, new):
    d = os.path.join(OUT, 'twin_' + name)
    if os.path.isdir(d):
        shutil.rmtree(d)
    os.makedirs(d)
    for p in glob.glob(os.path.join(FXDIR, '*')):
        if os.path.isfile(p):
            shutil.copy(p, d)
    f = os.path.join(d, filename)
    t = open(f, encoding='utf-8', newline='').read()
    pairs = list(zip(old, new)) if isinstance(old, (list, tuple)) else [(old, new)]
    for o, n in pairs:
        if t.count(o) != 1:
            raise SystemExit('twin %s: the text to change occurs %d times in %s (expected 1); our shader changed, update the twin' % (name, t.count(o), filename))
        t = t.replace(o, n)
    open(f, 'w', encoding='utf-8', newline='').write(t)
    return d

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--sample', action='store_true')
    ap.add_argument('--twins', action='store_true')
    ap.add_argument('--all', action='store_true')
    ap.add_argument('--probes', action='store_true', help='harness-limited effects with their one limited line probed (see PROBES)')
    ap.add_argument('--effects', default=None)
    ap.add_argument('--variant', default=None, help='regex on the variant name (with --effects)')
    ap.add_argument('--stage', default=None, choices=('vertex', 'fragment'))
    ap.add_argument('--jobs', type=int, default=6)
    ap.add_argument('--rounds', type=int, default=1)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--report', default=None)
    a = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    report = {'time': time.strftime('%Y-%m-%d %H:%M'), 'coverage': inventory_coverage()}
    code = 0
    if a.twins:
        units, plan = [], []
        for name, eff, fn, old, new, must, controls, why in TWINS:
            root = make_twin_root(name, fn, old, new)
            for kind, lst in (('must', must), ('control', controls)):
                for stage, var in lst:
                    for hx, vn in resolve(root, eff, stage, var):
                        units.append((root, eff, hx))
                        plan.append((name, kind, eff, hx, vn, why))
        # L13: the unmodified shaders as equals
        for eff, stage, var in L13_EQUALS:
            for hx, vn in resolve(FXDIR, eff, stage, var):
                units.append((FXDIR, eff, hx))
                plan.append(('L13_refind', 'equal', eff, hx, vn, 'our VLM / base output = the game\'s (L13b)'))
        res = run_units(units, a.jobs, a.rounds, a.seed)
        by = {}
        for r in res:
            by[(r['root'], r['effect'], r['hash'])] = r
        rows = []
        for name, kind, eff, hx, vn, why in plan:
            root = FXDIR if name == 'L13_refind' else os.path.join(OUT, 'twin_' + name)
            r = by.get((root, eff, hx))
            if r is None:
                ok, txt = False, 'no result'
            elif r['output'] in NOT_TESTED:
                ok, txt = False, 'not testable: %s %s' % (r['output'], r['detail'][:120])
            elif kind == 'must':
                ok, txt = (not r['equal']), ('CAUGHT in %s: %s' % (r['output'], r['detail'][:160])) if not r['equal'] else 'MISSED (equal)'
            else:
                ok, txt = r['equal'], 'equal' if r['equal'] else 'DIFFERS: %s %s' % (r['output'], r['detail'][:160])
            rows.append({'twin': name, 'role': kind, 'effect': eff, 'variant': vn, 'hash': hx, 'ok': ok, 'result': txt})
            print('%-4s %-28s %-8s %s/%s %s  %s' % ('OK' if ok else 'BAD', name, kind, eff, vn, hx, txt))
            if not ok:
                code = 1
        report['twins'] = rows
        print('twins: %d of %d rows as expected' % (sum(r['ok'] for r in rows), len(rows)))
    elif a.probes:
        units = []
        for name, eff, fn, old, new, why in PROBES:
            units.append((make_twin_root('probe_' + name, fn, old, new), eff, None))
        res = run_units(units, a.jobs, a.rounds, a.seed)
        tested, eq, diff, other = summarise(res, 'shader_equiv probes')
        report['results'] = res
        if diff or other:
            code = 1
    elif a.sample or a.effects or a.all:
        units = []
        if a.sample:
            for eff, stage, var in SAMPLE:
                for hx, vn in resolve(FXDIR, eff, stage, var):
                    units.append((FXDIR, eff, hx))
        else:
            effs = sorted(os.path.basename(p)[:-8] for p in glob.glob(os.path.join(FXDIR, '*.fx.json'))) if a.all else a.effects.split(',')
            for eff in effs:
                if a.variant or a.stage:
                    spec = load_spec(FXDIR, eff)
                    for stage in ('vertex', 'fragment'):
                        if a.stage and stage != a.stage:
                            continue
                        for v in spec.get(stage, []):
                            if a.variant and not re.search(a.variant, v['name']):
                                continue
                            units += [(FXDIR, eff, h.upper().zfill(16)) for h in v.get('hashes', [])]
                else:
                    units.append((FXDIR, eff, None))
        print('%d units' % len(units), flush=True)
        res = run_units(units, a.jobs, a.rounds, a.seed)
        tested, eq, diff, other = summarise(res, 'shader_equiv')
        report['results'] = res
        report['tested'], report['equal'], report['differ'], report['not_testable'] = len(tested), len(eq), len(diff), len(other)
        c = report['coverage']
        print('inventory: %d hashes in %d effects; %d of them sit in the %d effects we hand-wrote (a .fx.json); effects with none: %s' % (
            c['inventory_hashes'], c['effects_in_inventory'], c['in_effects_with_a_fx_json'], c['effects_with_fx_json'], ', '.join(c['without_fx_json'])))
        if diff or other:
            code = 1
    else:
        ap.print_help()
        return 2
    rp = a.report or os.path.join(OUT, 'report_%s.json' % ('twins' if a.twins else 'sample' if a.sample else 'all' if a.all else 'effects'))
    json.dump(report, open(rp, 'w'), indent=1)
    print('report:', rp)
    return code

if __name__ == '__main__':
    sys.exit(main())
