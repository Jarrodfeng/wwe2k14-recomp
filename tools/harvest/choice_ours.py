#!/usr/bin/env python3
r"""S103 WGD: OUR side of the whole-game choice diff (docs\agents\choice_diff.md), read from the REAL draw, offline, no game running.

Per arena and look: tools\scene\arena_rec.exe makes the arena's record from the player's files (the game-side slot provider, the look by its scene word, no camera), then
tools\scene\frame_rig.exe draws it through the real loader, appliers and front end with WWE_RDR_FEATURE_DUMP on. The front end writes one "d2" line per mesh and look where the
draw is final (src\renderer_scene\front_end.cpp, emit: the pair, the path that won, blend, alpha test + ref, depth write, cull, samplers) and the read rule's "d1" line (its why).
This tool turns those lines into one JSON line per mesh in the predictor's CHOICE vocabulary (tools\harvest\choice_predict.py predict_mesh): vs, ps, defines, blend, wmask, alpha,
plus zwrite, cull, samplers, path, why and the raw d2 fields.

  py -I tools\harvest\choice_ours.py [--arenas 4,44 | all] [--looks entrance,match] [--out DIR] [--env NAME=VALUE ...] [--jobs 2] [--game DIR]

--env sets a plant for the rig (e.g. WWE_RDR_L10_PLANT=1 WWE_RDR_D1_OLD=1): the planted twins of WGD1 section 5. At most 2 rig processes at once (CLAUDE.md: the PC is shared).
Writes DIR\aNN_<look>.jsonl (one line per mesh) and DIR\ours_summary.json. Scratch under out\ only: names, numbers and our variant names, nothing from the game's files.
Exit 0 every arena made; 1 any arena failed (named)."""
import argparse
import collections
import concurrent.futures
import glob
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'scene'))
sys.path.insert(0, HERE)

FX_DIR = os.path.join(ROOT, 'src', 'renderer', 'shaders', 'fx')
SCENE_WORD = {'entrance': 4, 'match': 1}     # arena_rec --scene-word: the game hook's own look route (arena_rec.cpp header)
COMPARE = ['NEVER', 'LESS', 'EQUAL', 'LEQUAL', 'GREATER', 'NOTEQUAL', 'GEQUAL', 'ALWAYS']   # VkCompareOp = the Xenos function bits (front_end.cpp AlphaDynamic)
# match arenas (WGD2 section 6: db\ids\arenas.json, ids 0-9, 11-17, 21-23, 28-56)
MATCH_ARENAS = list(range(0, 10)) + list(range(11, 18)) + list(range(21, 24)) + list(range(28, 57))


def main_checkout():
    """The checkout that holds game_data (a worktree has none): RDR_MAIN_CHECKOUT, else walk up from a .claude\\worktrees\\X tree."""
    env = os.environ.get('RDR_MAIN_CHECKOUT')
    if env:
        return env
    p = ROOT
    for _ in range(4):
        if os.path.isdir(os.path.join(p, 'game_data', 'extracted')):
            return p
        p = os.path.dirname(p)
    return ROOT


def family(slot, grp=None):
    """The draw family of a mesh by its pac key: stage slots 0..399, the movie keys 800-809, the ring keys 951-982, the flash part 31000; anything else (props of other pacs) other."""
    if 0 <= slot < 400:
        return 'stage'
    if 800 <= slot <= 809:
        return 'screen'
    if 951 <= slot <= 982:
        return 'ring'
    if slot == 31000:
        return 'flash'
    return 'other'


class FxDefines:
    """Our fx.json: variant name -> its defines (the same rule as choice_predict.FxJson)."""
    def __init__(self, fx_dir=FX_DIR):
        self.v, self.eff = {}, {}
        for fn in sorted(glob.glob(os.path.join(fx_dir, '*.fx.json'))):
            d = json.load(open(fn, encoding='utf-8'))
            for stage in ('vertex', 'fragment'):
                for v in d.get(stage, []):
                    nm = '%s_%s_%s' % (d['effect'], v['name'], stage)
                    self.v[nm] = sorted(v.get('defines', []))
                    self.eff[nm] = d['effect']

    def defines(self, name):
        return self.v.get(name or '', [])

    def effect(self, name):
        """the effect file a variant name belongs to ('' when the name is not ours)"""
        return self.eff.get(name or '', '')


def blend_text(d):
    """our draw state -> the predictor's blend text (choice_predict.rule_blend_alpha): off / on SRC/DST/OP (colour part only; ours keeps the alpha factors ONE / INV_SRC_ALPHA)."""
    if d.get('ben', -1) < 0:
        return None
    if not d['ben']:
        return 'off'
    return 'on SRC_ALPHA/%s/ADD' % ('ONE' if d['bdst'] == 1 else 'ONE_MINUS_SRC_ALPHA' if d['bdst'] == 7 else 'F%d' % d['bdst'])


def alpha_text(d):
    if d.get('at', 0) <= 0:
        return 'off' if d.get('ben', -1) >= 0 else None
    return '%s %.6g' % (COMPARE[d['af'] & 7], d['ar'] / 255.0)


def to_row(d, d1, fx, arena, look):
    vs, ps = d.get('vs', ''), d.get('ps', '')
    row = dict(arena=arena, look=look, file=file_key(d, arena) if d['slot'] >= 0 else '%s' % d.get('part'), slot=d['slot'], src_pac=d.get('src_pac'), mesh=d['mesh'], key=d.get('key'),
               part=d.get('part'), family=family(d['slot']), effect=d.get('shader'), path=d.get('path'), vs=vs, ps=ps,
               vs_effect=fx.effect(vs), ps_effect=fx.effect(ps), vs_defines=fx.defines(vs), ps_defines=fx.defines(ps),
               defines=sorted(set(fx.defines(vs)) | set(fx.defines(ps))), blend=blend_text(d), wmask=15, alpha=alpha_text(d),
               zwrite=d.get('dw'), cull=d.get('cull'), ff=d.get('ff'), samplers=d.get('samp', {}), out_alpha=d.get('oa'), lm_mode=d.get('lm'),
               raw={k: d[k] for k in ('blend', 'ben', 'bdst', 'at', 'af', 'ar', 'spec_lev', 'no_hl', 'ablend', 'add', 'inl', 'cutout', 'mat_at', 'amask', 'cull_back', 'rule_tried', 'grp', 'phase1', 'ent',
                                      'vs_oa', 'vc', 'mdl_a', 'amb_w', 'dif_w', 'dal', 'cword') if k in d})
    if d1:
        row['rule'] = dict(why=d1.get('why'), effect=d1.get('effect'), tech=d1.get('tech'), pass_rule=d1.get('pass'), pass_used=d1.get('pass_used'), shadow=d1.get('shadow'), vs=d1.get('vs'), ps=d1.get('ps'), bound=d1.get('rule'))
    return row


def parse_dump(path):
    """-> (d2 lines of the FIRST Record that wrote any, d1 lines keyed by (part, slot, mesh, phase1)). The front end writes one d2 line per mesh per Record and none from
    RenderToPixels' seed frame; the rig then records the whole frame (and, with --per-family, a panel per family): the first Record with d2 lines is the whole frame."""
    d2, d1 = [], {}
    if not os.path.exists(path):
        return d2, d1
    block, taken = 0, None
    for ln in open(path, encoding='utf-8', errors='replace'):
        if '"name":"frame_begin"' in ln:
            block += 1
            continue
        if '"k":"d2"' in ln or '"k":"d1"' in ln:
            try:
                j = json.loads(ln)
            except ValueError:
                continue
            if j['k'] == 'd2':
                if taken is None:
                    taken = block
                if block == taken:
                    d2.append(j)
            else:
                d1.setdefault((j['part'], j['slot'], j['mesh'], j['phase1']), j)
    return d2, d1


def join_mirrored(rows, d2):
    """SCU: the front end writes a second d2 line for a mesh's mirrored copy (`mir` 1: an `r` slot's copy, the placement's 3x3 determinant < 0). One row per mesh: the base copy's
    line, with the mirrored copy's cull and front face as cull_mirror / ff_mirror (choice_diff compares them with the game's copy_mode). A mesh with only a mirrored line keeps it."""
    by_key = {}
    for r, d in zip(rows, d2):
        k = (d.get('part'), d.get('slot'), d.get('mesh'))
        if d.get('mir'):
            by_key.setdefault(k, {})['mir'] = r
        else:
            by_key.setdefault(k, {})['base'] = r
    res = []
    for k, v in by_key.items():
        base, mir = v.get('base'), v.get('mir')
        if base is None:
            res.append(mir)
            continue
        if mir is not None:
            base['cull_mirror'], base['ff_mirror'] = mir.get('cull'), mir.get('ff')
        res.append(base)
    return res


def file_key(d, arena):
    """the join key's file: the mesh's own source pac (d2 src_pac) and its slot; a line without src_pac falls back to the arena pac"""
    sp = os.path.basename((d.get('src_pac') or '').replace('\\', '/')).lower()
    if not sp:
        sp = 'bg%02d.pac' % arena
    return '%s#%d' % (sp, d['slot'])


def run_one(arena, look, out_dir, env_extra, game, fx, session, show_all=False):
    """arena_rec + frame_rig for one (arena, look) -> (rows, problem text or None)"""
    tools = os.path.join(ROOT, 'out', 'tools')
    work = os.path.join(out_dir, 'work')
    os.makedirs(work, exist_ok=True)
    tag = 'a%02d_%s' % (arena, look)
    rsc = os.path.join(work, tag + '.rsc')
    dump = os.path.join(work, tag + '_dump.jsonl')
    if os.path.exists(dump):
        os.remove(dump)
    p = subprocess.run([os.path.join(tools, 'arena_rec.exe'), '--game', game, '--arena', str(arena), '--from-provider', '--scene-word', str(SCENE_WORD[look]),
                        '--camera-from', 'none', '--out', rsc], capture_output=True, text=True, errors='replace', timeout=300)
    if p.returncode != 0:
        return [], 'arena_rec exit %d: %s' % (p.returncode, (p.stdout + p.stderr).strip()[-300:])
    env = dict(os.environ)
    env.update(env_extra)
    env['WWE_RDR_FEATURE_DUMP'] = dump
    # the camera does not change any choice (no frustum cull in FrontEnd::Record: every recorded chunk is emitted); a small fixed one keeps the draw cheap
    cmd = [os.path.join(tools, 'frame_rig.exe'), '--record', rsc, '--game', game, '--size', '160x90', '--prefix', os.path.join(work, tag),
           '--camera', '0.001,0,0,0,0,0.001,0,0,0,0,0.0001,0,0,0,0.5,1']
    if look == 'entrance':
        cmd.append('--look-entrance')
    if show_all:   # every movie key shown (TOLD through frame_rig --show): reaches keys no LegacyShow set holds (803); the stage art keys keep the look's rule
        cmd += ['--show', ','.join(str(k) for k in range(800, 810))]
    r = session.rig(cmd, env=env, timeout=600)
    if r.returncode != 0:
        return [], 'frame_rig exit %d: %s' % (r.returncode, (r.stdout or '').strip()[-300:])
    d2, d1 = parse_dump(dump)
    rows = join_mirrored([to_row(d, d1.get((d['part'], d['slot'], d['mesh'], d.get('phase1'))), fx, arena, look) for d in d2], d2)
    for f in (dump, rsc):
        try:
            os.remove(f)
        except OSError:
            pass
    for f in glob.glob(os.path.join(work, tag + '_*.png')) + glob.glob(os.path.join(work, tag + '_report.txt')):
        os.remove(f)
    if not rows:
        return [], 'no d2 line (the rig drew nothing, or the dump switch is not in this build)'
    return rows, None


def parse_env(items):
    out = {}
    for it in items or []:
        for tok in it.split():
            k, _, v = tok.partition('=')
            out[k] = v or '1'
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--arenas', default='all')
    ap.add_argument('--looks', default='entrance,match')
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 'wgd', 'ours'))
    ap.add_argument('--env', action='append')
    ap.add_argument('--jobs', type=int, default=2)
    ap.add_argument('--game')
    ap.add_argument('--show-all-screens', action='store_true', help='tell the rig every movie key 800-809 is shown (frame_rig --show): also reaches key 803, which no LegacyShow set holds')
    a = ap.parse_args()
    import frame_rig   # tools\scene\frame_rig.py: RigSession (one-shot jobs here: the dump file is per process, so per job)
    arenas = MATCH_ARENAS if a.arenas == 'all' else [int(x) for x in a.arenas.split(',')]
    looks = a.looks.split(',')
    env_extra = parse_env(a.env)
    game = a.game or os.path.join(main_checkout(), 'game_data', 'extracted')
    os.makedirs(a.out, exist_ok=True)
    fx = FxDefines()
    session = frame_rig.RigSession(serve=False)
    jobs = [(ar, lk) for ar in arenas for lk in looks]
    problems, counts = {}, collections.Counter()
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, min(2, a.jobs))) as ex:   # at most 2 rig processes alive
        futs = {ex.submit(run_one, ar, lk, a.out, env_extra, game, fx, session, a.show_all_screens): (ar, lk) for ar, lk in jobs}
        for f in concurrent.futures.as_completed(futs):
            ar, lk = futs[f]
            try:
                rows, prob = f.result()
            except Exception as e:   # noqa: BLE001 (named per arena below)
                rows, prob = [], 'exception %r' % e
            with open(os.path.join(a.out, 'a%02d_%s.jsonl' % (ar, lk)), 'w', encoding='utf-8') as fo:
                for r in rows:
                    fo.write(json.dumps(r, sort_keys=True) + '\n')
            counts[lk] += len(rows)
            if prob:
                problems['a%02d_%s' % (ar, lk)] = prob
                print('FAIL a%02d %s: %s' % (ar, lk, prob))
    summ = dict(arenas=arenas, looks=looks, env=env_extra, show_all_screens=a.show_all_screens, rows=dict(counts), problems=problems, rig_jobs=session.stats)
    json.dump(summ, open(os.path.join(a.out, 'ours_summary.json'), 'w'), indent=1, sort_keys=True)
    print('ours: %d arenas x %d looks, %s mesh rows, %d failed, env %s -> %s' % (len(arenas), len(looks), dict(counts), len(problems), env_extra or '{}', a.out))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
