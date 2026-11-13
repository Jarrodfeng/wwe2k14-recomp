r"""Lane 33 (S101): the game's light constants (lm_extract.py) against what the Arena Viewer evaluates (bank_dump), per light set and light.

  py -I tools\lightmatch\lm_compare.py entrance --case ZIP --arena 4 --wrestler 324 [--frames 10,50,90] [--step 6] [--label bryan]
  py -I tools\lightmatch\lm_compare.py match --case ZIP --arena N ...      (the viewer's match look: stage key sets; see --help)

entrance: (1) one scan of the viewer's entrance timeline (every cue, every STEP local frames) records the light groups the viewer feeds,
with a fixed camera (colours do not depend on it); (2) per game frame the clock whose groups fit the game's best (sum over classes of the
nearest-group colour distance) is chosen, then refined frame by frame around it; (3) the viewer is run again at the game's own camera
(c56..c59 / c72) and that clock, and every game group is matched to the nearest viewer group of the same class: per light the angle
between the directions (degrees), the colour ratio, and per set the sky / ground ratios. Output out\s101\lightmatch\<label>\compare.json
(game values: local only) and a table of counts and names on stdout.

Dominant difference = the term (direction / colour / ambient / limit) with the largest share of the total cost, where a direction error costs
(angle in radians) x (the light's luminance) and a colour error its absolute RGB difference.
"""
import argparse
import json
import math
import os
import re
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import lm_viewer as V  # noqa: E402

MAIN = V.MAIN
OUT = MAIN / 'out' / 's101' / 'lightmatch'
FIT_CLASSES = ('wrestler', 'arena', 'prop', 'crowd')


def game_class(effect, klass):
    first = effect.split('|')[0]
    return V.klass_of(first, crowd=first.startswith('yAud'))


def norm_game_group(g):
    """game group (lm_extract) -> {klass, effect, n, dir[4][3], col[4][3], sky, ground, limit, hl} or None when it carries no light constants."""
    v = g['vals']

    def pick(name):
        if name not in v:
            return None
        d = v[name]
        best = max(d.values(), key=len)  # (the longest run: the fragment bank of a skin draw has 5 lights, a fragment copy of an arena set 1)
        return best

    col, vec, sky, gnd = pick('DifLghCol'), pick('DifLghVec'), pick('SkyCol'), pick('GroundCol')
    if col is None and sky is None:
        return None
    out = dict(klass=game_class(g['effect'], g['klass']), effect=g['effect'], n=g['draws'], dir=[[0.0] * 3 for _ in range(4)],
               col=[[0.0] * 3 for _ in range(4)], sky=[0.0] * 3, ground=[0.0] * 3, limit=None, hl_col=None, hl_dir=None,
               has_lights=bool(col and vec))  # (a draw variant that carries no DifLghCol / DifLghVec, e.g. yCh_gear 'base': judged on sky / ground only)
    if col:
        for i in range(min(4, len(col) // 4)):
            out['col'][i] = col[i * 4:i * 4 + 3]
        if len(col) >= 20:
            out['hl_col'] = col[16:19]
    if vec:
        for i in range(min(4, len(vec) // 4)):
            out['dir'][i] = vec[i * 4:i * 4 + 3]
        if len(vec) >= 20:
            out['hl_dir'] = vec[16:19]
    if sky:
        out['sky'] = sky[:3]
    if gnd:
        out['ground'] = gnd[:3]
    lim = pick('LightLimit') or pick('fvLightLimit')
    out['limit'] = lim[:3] if lim else None
    return out


def lum(c):
    return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]


def is_off(g):
    return max(max(c) for c in g['col']) < 1e-4 and abs(sum(g['sky']) - sum(g['ground'])) < 1e-3


def color_dist(g, v):
    d = 0.0
    for k in range(3 if g.get('has_lights', True) else 0):
        d += sum(abs(a - b) for a, b in zip(g['col'][k], v['col'][k]))
    d += sum(abs(a - b) for a, b in zip(g['sky'], v['sky'])) + sum(abs(a - b) for a, b in zip(g['ground'], v['ground']))
    return d


def angle_deg(a, b):
    na, nb = math.sqrt(sum(x * x for x in a)), math.sqrt(sum(x * x for x in b))
    if na < 1e-6 or nb < 1e-6:
        return None
    c = max(-1.0, min(1.0, sum(x * y for x, y in zip(a, b)) / (na * nb)))
    return math.degrees(math.acos(c))


def fit_cost(game, vgroups, classes=FIT_CLASSES):
    """Sum over the game's lit groups of the nearest same-class viewer group's colour distance (the mean per class, summed)."""
    tot, cnt = 0.0, 0
    per = {}
    for k in classes:
        gs = [g for g in game if g['klass'] == k and not is_off(g)]
        vs = [v for v in vgroups if v['klass'] == k and not is_off(v)]
        if not gs:
            continue
        if not vs:
            per[k] = 9.9
            tot += 9.9
            cnt += 1
            continue
        c = sum(min(color_dist(g, v) for v in vs) for g in gs) / len(gs)
        per[k] = c
        tot += c
        cnt += 1
    return tot, per


def pair_diff(g, v):
    """The differences of one game group against one viewer group. Costs in the units documented above."""
    lights = []
    cost_dir = cost_col = 0.0
    for k in range(4 if g.get('has_lights', True) else 0):
        gl, vl = lum(g['col'][k]), lum(v['col'][k])
        if gl < 1e-4 and vl < 1e-4:
            continue
        ang = angle_deg(g['dir'][k], v['dir'][k]) if (gl >= 1e-4 and vl >= 1e-4) else None
        ratio = (vl / gl) if gl > 1e-4 else None
        cdiff = sum(abs(a - b) for a, b in zip(g['col'][k], v['col'][k]))
        lights.append(dict(light=k, angle_deg=None if ang is None else round(ang, 3), lum_game=round(gl, 4), lum_viewer=round(vl, 4),
                           ratio=None if ratio is None else round(ratio, 4), col_abs=round(cdiff, 4)))
        cost_col += cdiff
        if ang is not None:
            cost_dir += math.radians(ang) * max(gl, vl)
    sky_d = sum(abs(a - b) for a, b in zip(g['sky'], v['sky']))
    gnd_d = sum(abs(a - b) for a, b in zip(g['ground'], v['ground']))
    cost_amb = sky_d + gnd_d
    cost_lim = 0.0  # (the light limit is reported in the page, not scored: min(cube x DifLghMax, limit) never reaches 10; see LIGHT_MATCH.md)
    terms = dict(direction=cost_dir, colour=cost_col, ambient=cost_amb, limit=cost_lim)
    return dict(lights=lights, sky_abs=round(sky_d, 4), ground_abs=round(gnd_d, 4), sky_ratio=round(lum(v['sky']) / max(lum(g['sky']), 1e-4), 4),
                ground_ratio=round(lum(v['ground']) / max(lum(g['ground']), 1e-4), 4), terms={k: round(x, 4) for k, x in terms.items()},
                dominant=max(terms, key=terms.get), cost=round(sum(terms.values()), 4))


def match_groups(game, vgroups):
    res = []
    for g in game:
        if g['klass'] not in FIT_CLASSES or is_off(g):
            continue
        cands = [v for v in vgroups if v['klass'] == g['klass'] and not is_off(v)]
        if not cands:
            res.append(dict(effect=g['effect'], klass=g['klass'], n=g['n'], matched=False))
            continue
        # nearest by the full cost (colour + direction), not colour alone: two sets can share colours
        best = min(((pair_diff(g, v), v) for v in cands), key=lambda t: t[0]['cost'])
        d, v = best
        res.append(dict(effect=g['effect'], klass=g['klass'], n=g['n'], matched=True, viewer_effect=v['effect'], viewer_sets=v['sets'], **d))
    return res


def summarise(matches):
    """-> {terms: total cost per term, dominant: name, per_class: {...}}"""
    terms = dict(direction=0.0, colour=0.0, ambient=0.0, limit=0.0)
    per = {}
    for m in matches:
        if not m.get('matched'):
            continue
        w = 1.0
        for k, x in m['terms'].items():
            terms[k] += x * w
        pc = per.setdefault(m['klass'], dict(groups=0, cost=0.0, worst_angle=0.0))
        pc['groups'] += 1
        pc['cost'] += m['cost']
        for l in m['lights']:
            if l['angle_deg'] is not None:
                pc['worst_angle'] = max(pc['worst_angle'], l['angle_deg'])
    dom = max(terms, key=terms.get) if any(terms.values()) else 'none'
    return dict(terms={k: round(x, 4) for k, x in terms.items()}, dominant=dom, per_class=per,
                unmatched=sum(1 for m in matches if not m.get('matched')))


def game_frames(frame_dir, want):
    fs = {}
    for p in sorted(Path(frame_dir).glob('frame_*.json')):
        n = int(re.search(r'frame_(\d+)', p.name).group(1))
        if want and n not in want:
            continue
        j = json.loads(p.read_text(encoding='utf-8'))
        groups = [x for x in (norm_game_group(g) for g in j['groups']) if x]
        fs[n] = dict(camera=j['camera'], groups=groups, guest_frame=j['guest_frame'])
    if not fs:
        raise SystemExit(f'FAIL  no game frames in {frame_dir} (run lm_extract.py first)')
    return fs


def entrance_cmds(arena, wrestler, attire=0):
    return [{'cmd': 'look', 'mode': 'lit', 'shadows': False, 'dof': False, 'fxaa': False, 'bloom': False, 'glare': False, 'gamma': False},
            {'cmd': 'load', 'kind': 'arena', 'id': arena},
            {'cmd': 'entrance', 'wrestler': wrestler, 'attire': attire, 'play': False, 'camera': 'free'}]


def scan_clock(arena, wrestler, cue_len, cam_vp, step, tag, env=None, backend='vk', chunk=80):
    """Dumps the viewer's groups at every (cue, local frame) of the timeline. Returns [(cue, f, groups)]."""
    wd = MAIN / 'out' / 's101' / 'lightmatch' / '_scan' / tag
    wd.mkdir(parents=True, exist_ok=True)
    clocks = [(cue, f) for cue, ln in cue_len for f in range(0, ln, step)]
    out = []
    for c0 in range(0, len(clocks), chunk):
        part = clocks[c0:c0 + chunk]
        cmds = entrance_cmds(arena, wrestler) + [{'cmd': 'camera', 'viewproj': cam_vp, 'eye': [0, 0, 0]}]
        for i, (cue, f) in enumerate(part):
            cmds += [{'cmd': 'entrance_time', 'cue': cue, 'f': f, 'pause': True},
                     {'cmd': 'bank_dump', 'path': str(wd / f'b{i}.json'), 'w': 1280, 'h': 720}]
        cmds.append({'cmd': 'quit'})
        V.run(cmds, backend=backend, workdir=wd, tag='scan', env_extra=env)
        for i, (cue, f) in enumerate(part):
            p = wd / f'b{i}.json'
            if not p.exists():
                raise SystemExit(f'FAIL  the viewer wrote no dump for cue {cue} frame {f}')
            out.append((cue, f, V.groups_from_bank(json.loads(p.read_text(encoding='utf-8')))))
            p.unlink()
    (wd / 'scan.json').write_text(json.dumps([[c, f, g] for c, f, g in out]), encoding='utf-8')  # (kept for offline fits: game values never; the viewer's own)
    return out


def viewer_at(arena, wrestler, clocks, cam, tag, env=None, backend='vk'):
    """clocks = [(cue, f, camera dict)]; returns the groups at each (one viewer run)."""
    wd = MAIN / 'out' / 's101' / 'lightmatch' / '_scan' / tag
    wd.mkdir(parents=True, exist_ok=True)
    cmds = entrance_cmds(arena, wrestler)
    for i, (cue, f, c) in enumerate(clocks):
        cmds += [{'cmd': 'camera', 'viewproj': c['viewproj_rows'], 'eye': c.get('eye', [0, 0, 0])},
                 {'cmd': 'entrance_time', 'cue': cue, 'f': f, 'pause': True},
                 {'cmd': 'bank_dump', 'path': str(wd / f'a{i}.json'), 'w': 1280, 'h': 720}]
    cmds.append({'cmd': 'quit'})
    V.run(cmds, backend=backend, workdir=wd, tag='at', env_extra=env)
    res = []
    for i in range(len(clocks)):
        p = wd / f'a{i}.json'
        res.append(V.groups_from_bank(json.loads(p.read_text(encoding='utf-8'))))
        p.unlink()
    return res


def cue_lengths(arena, wrestler):
    rep, _ = V.run(entrance_cmds(arena, wrestler) + [{'cmd': 'quit'}], tag='cues')
    ent = next((r for r in rep if 'cues' in r and 'wrestler' in r), None)
    if not ent:
        raise SystemExit('FAIL  the viewer did not report an entrance for this wrestler: ' + json.dumps(rep)[:300])
    return [(c['cue'], c['len']) for c in ent['cues']]


def run_entrance(a):
    fr_dir = Path(a.frames_dir) if a.frames_dir else OUT / a.label
    want = {int(x) for x in a.frames.split(',')} if a.frames else None
    frames = game_frames(fr_dir, want)
    result = dict(label=a.label, arena=a.arena, wrestler=a.wrestler, frames={})
    picks = {}
    some = next(iter(frames.values()))
    if a.clock:  # pinned clocks (frame:cue:local,...): no scan (the gate re-checks the fit at a clock an earlier full search found)
        for item in a.clock.split(','):
            n, cue, fl = (int(x) for x in item.split(':'))
            picks[n] = (None, cue, fl)
    else:
        cl = cue_lengths(a.arena, a.wrestler)
        print('cues (cue, local frames):', cl)
        scan = scan_clock(a.arena, a.wrestler, cl, some['camera']['viewproj_rows'], a.step, a.label + '_scan', env=a_env(a))
        print(f'scanned {len(scan)} clocks')
        for n, f in frames.items():
            costs = [(fit_cost(f['groups'], vg)[0], cue, fl) for cue, fl, vg in scan]
            costs.sort(key=lambda t: t[0])
            picks[n] = costs[0]
        # refine: +-step frames around the coarse pick, frame by frame
        for n, f in frames.items():
            c0, cue, fl = picks[n]
            cand = [(cue, x) for x in range(max(0, fl - a.step), fl + a.step + 1) if x < dict(cl)[cue]]
            gs = viewer_at(a.arena, a.wrestler, [(cc, x, some['camera']) for cc, x in cand], some['camera'], a.label + '_ref', env=a_env(a))
            best = min(((fit_cost(f['groups'], g)[0], cc, x) for (cc, x), g in zip(cand, gs)), key=lambda t: t[0])
            picks[n] = best
            print(f'frame {n}: best clock cue {best[1]} local {best[2]}, fit cost {best[0]:.4f} (coarse {c0:.4f})')
    for n, f in frames.items():
        fit, cue, fl = picks[n]
        cam = dict(f['camera'])
        vg = viewer_at(a.arena, a.wrestler, [(cue, fl, cam)], cam, a.label + '_cmp', env=a_env(a))[0]
        matches = match_groups(f['groups'], vg)
        s = summarise(matches)
        total, per = fit_cost(f['groups'], vg)
        fit = total if fit is None else fit
        result['frames'][str(n)] = dict(cue=cue, local=fl, fit_cost=round(fit, 4), fit_per_class=per, summary=s, matches=matches)
        print(f"frame {n}: dominant {s['dominant']}, terms {s['terms']}, unmatched {s['unmatched']}")
    p = fr_dir / 'compare.json'
    if a.out_name:
        p = fr_dir / a.out_name
    p.write_text(json.dumps(result, indent=1), encoding='utf-8')
    print('OK ->', p)
    return result


# ---- match look ----------------------------------------------------------------------------------------------------------------------
BG_IDS = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 12, 13, 14, 15, 16, 17, 21, 22, 23, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44,
          45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 66, 67, 68, 76, 77, 78, 79, 80]


EXTRA_CMDS = []  # viewer commands inserted right after the arena load (--viewer-cmd, gate controls)
AFTER_ENTRANCE = [0]  # wrestler id: start and stop an entrance before the dump (--after-entrance)


def match_cmds(arena, key_time, wrestlers=(100, 101)):
    c = [{'cmd': 'look', 'mode': 'lit', 'shadows': False, 'dof': False, 'fxaa': False, 'bloom': False, 'glare': False, 'gamma': False}]
    if key_time is not None:
        c.append({'cmd': 'lights', 'time': key_time, 'mode': 'match'})
    c.append({'cmd': 'load', 'kind': 'arena', 'id': arena})
    c += [dict(x) for x in EXTRA_CMDS]
    for i, w in enumerate(wrestlers):
        c.append({'cmd': 'place', 'wrestler': w, 'attire': 0, 'spot': 'ring', 'id': f'w{i + 1}', 'motion': 'idle', 'frame': 30})
    return c


def match_dumps(items, cam, tag, env=None, backend='vk', chunk=40):
    """items = [(arena, key_time)] -> groups per item (one fresh arena load per item, in chunked viewer sessions)."""
    wd = MAIN / 'out' / 's101' / 'lightmatch' / '_scan' / tag
    wd.mkdir(parents=True, exist_ok=True)
    res = []
    for c0 in range(0, len(items), chunk):
        part = items[c0:c0 + chunk]
        cmds = []
        for i, (arena, t) in enumerate(part):
            cmds += match_cmds(arena, t)
            cmds += [{'cmd': 'camera', 'viewproj': cam['viewproj_rows'], 'eye': cam.get('eye', [0, 0, 0])}]
            if AFTER_ENTRANCE[0]:  # (no camera move after the stop: the lights must follow the camera that was already there)
                cmds += [{'cmd': 'entrance', 'wrestler': AFTER_ENTRANCE[0], 'attire': 0, 'play': False, 'camera': 'free'}, {'cmd': 'entrance_stop'}]
            cmds += [{'cmd': 'bank_dump', 'path': str(wd / f'm{i}.json'), 'w': 1280, 'h': 720}]
        cmds.append({'cmd': 'quit'})
        V.run(cmds, backend=backend, workdir=wd, tag='m', env_extra=env, timeout=900)
        for i in range(len(part)):
            p = wd / f'm{i}.json'
            if not p.exists():
                raise SystemExit(f'FAIL  the viewer wrote no dump for {part[i]}')
            res.append(V.groups_from_bank(json.loads(p.read_text(encoding='utf-8'))))
            p.unlink()
    return res


def run_match(a):
    fr_dir = Path(a.frames_dir) if a.frames_dir else OUT / a.label
    want = {int(x) for x in a.frames.split(',')} if a.frames else None
    frames = game_frames(fr_dir, want)
    some = next(iter(frames.values()))
    arena = a.arena
    if arena < 0:
        gs = match_dumps([(n, None) for n in BG_IDS], some['camera'], a.label + '_arenas', env=a_env(a))
        costs = sorted((fit_cost(some['groups'], g, ('arena', 'prop', 'crowd'))[0], n) for n, g in zip(BG_IDS, gs))
        print('arena fit (cost, id), best five:', [(round(c, 3), n) for c, n in costs[:5]])
        arena = costs[0][1]
    times = list(range(0, a.tmax + 1, a.tstep))
    gs = match_dumps([(arena, t) for t in times], some['camera'], a.label + '_times', env=a_env(a))
    result = dict(label=a.label, arena=arena, frames={})
    for n, f in frames.items():
        fits = sorted((fit_cost(f['groups'], g)[0], t) for t, g in zip(times, gs))
        best_t = fits[0][1]
        print(f'frame {n}: best key time {best_t}, fit {fits[0][0]:.4f}; next {fits[1:3]}')
        vg = match_dumps([(arena, best_t)], f['camera'], a.label + '_cmp', env=a_env(a))[0]
        matches = match_groups(f['groups'], vg)
        s = summarise(matches)
        total, per = fit_cost(f['groups'], vg)
        result['frames'][str(n)] = dict(key_time=best_t, fit_cost=round(total, 4), fit_per_class=per, summary=s, matches=matches)
        print(f"frame {n}: dominant {s['dominant']}, terms {s['terms']}, unmatched {s['unmatched']}")
    p = fr_dir / (a.out_name or 'compare.json')
    p.write_text(json.dumps(result, indent=1), encoding='utf-8')
    print('OK ->', p)
    return result


def a_env(a):
    return {'WWE_LM_PLANT': a.plant} if getattr(a, 'plant', '') else None


def main():
    ap = argparse.ArgumentParser()
    sp = ap.add_subparsers(dest='mode', required=True)
    e = sp.add_parser('entrance')
    e.add_argument('--label', required=True)
    e.add_argument('--arena', type=int, required=True)
    e.add_argument('--wrestler', type=int, required=True)
    e.add_argument('--frames', default='')
    e.add_argument('--frames-dir', default='')
    e.add_argument('--step', type=int, default=8)
    e.add_argument('--clock', default='', help='frame:cue:local,... pinned clocks (no scan)')
    e.add_argument('--plant', default='')
    e.add_argument('--out-name', default='')
    m = sp.add_parser('match')
    m.add_argument('--label', required=True)
    m.add_argument('--arena', type=int, default=-1, help='-1: find it by the fit over every arena')
    m.add_argument('--frames', default='')
    m.add_argument('--frames-dir', default='')
    m.add_argument('--tmax', type=int, default=600)
    m.add_argument('--tstep', type=int, default=20)
    m.add_argument('--plant', default='')
    m.add_argument('--viewer-cmd', default='', help='one JSON command sent after the arena load')
    m.add_argument('--after-entrance', type=int, default=0, help='wrestler id: entrance + entrance_stop before the dump')
    m.add_argument('--out-name', default='')
    a = ap.parse_args()
    if a.mode == 'entrance':
        run_entrance(a)
    else:
        if a.viewer_cmd:
            EXTRA_CMDS.append(json.loads(a.viewer_cmd))
        AFTER_ENTRANCE[0] = a.after_entrance
        run_match(a)


if __name__ == '__main__':
    main()
