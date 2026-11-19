#!/usr/bin/env python3
r"""S103 lane D1 check: the front end's stage-slot choice is the READ RULE (src\renderer_scene\read_rule.h), the same as the frozen predictor tools\harvest\choice_predict.py.
0 game runs: offline (out\tools\frame_rig.exe on a case's records, WWE_RDR_FEATURE_DUMP's "d1" lines + feature_diff's report).

  py -I tools\scene\d1_check.py --case out\s103\run29_case.zip --frames 450,500,600 [--arena 4] [--crop X0,Y0,X1,Y1]

  (1) RULE    every d1 line (one per stage-slot mesh the rig drew, per look) against choice_predict.predict(ARENA): effect alternative (+384 / +388), technique, pass
              (the shadow gate at the predictor's default). 0 mismatches = PASS. The pair (vs / ps) is compared too where our pass is the rule's (no stand-in).
  (2) DIFF    feature_diff on each frame, with the rule and with WWE_RDR_D1_OLD=1 (the old heuristics, the planted twin): the "EXTRA props FX_LIGHTMAP" and the missing
              arena FX_SHADOW lines must be gone with the rule and back with the plant.
  (3) CROP    (with --crop, the rig picture's region, pixels at the rig size): our mean luminance with the rule / with the plant / the game's picture in the case (when the
              case carries one for the frame), and the ratios.
Exit 0 = PASS (1) and (2)."""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, 'tools', 'harvest'))
import feature_diff as F  # noqa: E402

TOKENS = (('EXTRA props FX_LIGHTMAP', lambda t: 'props' in t and 'FX_LIGHTMAP' in t and 'ours uses' in t),
          ('arena FX_SHADOW missing', lambda t: t.startswith('arena: the game uses FX_SHADOW') and 'ours on none' in t))


def d1_lines(dump):
    out = []
    if not os.path.exists(dump):
        return out
    for line in open(dump, encoding='utf-8'):
        try:
            j = json.loads(line)
        except Exception:
            continue
        if j.get('k') == 'd1':
            out.append(j)
    return out


def texts(res):
    return [m['what'] for m in res['missing']] + [m['what'] for m in res['values']]


def lum(path, box):
    try:
        from PIL import Image
    except ImportError:
        return None
    if not path or not os.path.exists(path):
        return None
    im = Image.open(path).convert('RGB')
    x0, y0, x1, y1 = box
    if im.size != (1280, 720):
        sx, sy = im.size[0] / 1280.0, im.size[1] / 720.0
        x0, x1, y0, y1 = int(x0 * sx), int(x1 * sx), int(y0 * sy), int(y1 * sy)
    px = list(im.crop((x0, y0, x1, y1)).getdata())
    return sum(0.2126 * r + 0.7152 * g + 0.0722 * b for r, g, b in px) / max(1, len(px))


def find_png(prefix):
    for suf in ('_whole.png', '.png', '_frame.png', '_0.png', '_000.png'):   # the rig's frame picture (_whole), never a map (_map_*)
        if os.path.exists(prefix + suf):
            return prefix + suf
    d, b = os.path.split(prefix)
    if os.path.isdir(d):
        c = sorted(x for x in os.listdir(d) if x.startswith(b) and x.endswith('.png'))
        if c:
            return os.path.join(d, c[0])
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--case', default='out/s103/run29_case.zip')
    ap.add_argument('--frames', default='450,500,600')
    ap.add_argument('--arena', type=int, default=4)
    ap.add_argument('--crop', default='')
    ap.add_argument('--game-png', default='', help='the game picture for --crop (default: the case\'s own for the frame, when it has one)')
    a = ap.parse_args()
    case_path = a.case if os.path.isabs(a.case) or os.path.exists(a.case) else os.path.join(F.main_checkout(), a.case)
    if not os.path.exists(case_path):
        print('FAIL  no case at ' + case_path)
        return 1
    frames = [int(x) for x in a.frames.split(',') if x]
    fx = F.Fx()
    case = F.Case(case_path)
    out = os.path.join(ROOT, 'out', 's103', 'D1', 'check')
    base = F.run_frames(case, fx, frames, out, {}, tag='rule')
    plant = F.run_frames(case, fx, frames, out, {'WWE_RDR_D1_OLD': '1'}, tag='old')

    # (1) the rule against the predictor
    import choice_predict as P
    table, _B = P.predict(a.arena, ['entrance', 'match'], {})
    pred = {(r['slot'], r['mesh']): r for r in table['meshes']}
    n = mism = pair_n = pair_mism = rule_drawn = 0
    seen = set()
    bad = []
    for fr in frames:
        for j in d1_lines(os.path.join(out, 'rule_%d_dump.jsonl' % fr)):
            key = (j['slot'], j['mesh'], j['phase1'])
            if key in seen:
                continue
            seen.add(key)
            look = 'match' if j['phase1'] else 'entrance'
            row = pred.get((j['slot'], j['mesh']))
            if row is None:
                bad.append('slot %d mesh %d: not in the predictor' % (j['slot'], j['mesh']))
                mism += 1
                n += 1
                continue
            g = row['game'][look]
            n += 1
            rule_drawn += j['rule']
            ours = (j['array'], j['tech'], j['pass'])
            theirs = (g.get('array'), g.get('technique'), g.get('pass_index'))
            if ours != theirs:
                mism += 1
                bad.append('slot %d mesh %d %s (%s): ours array/tech/pass %s, predictor %s (%s)' % (j['slot'], j['mesh'], look, j['shader'], ours, theirs, j['why'] or 'rule'))
            ch = row['looks'][look]['choices']
            if j['rule'] and j['pass_used'] == j['pass'] and ch:
                pair_n += 1
                if (j['vs'], j['ps']) != (ch[0]['vs'], ch[0]['ps']):
                    pair_mism += 1
                    bad.append('slot %d mesh %d %s: pair %s + %s, predictor %s + %s' % (j['slot'], j['mesh'], look, j['vs'], j['ps'], ch[0]['vs'], ch[0]['ps']))
    print('(1) RULE  %d stage-slot meshes x looks compared: %d mismatches on alternative / technique / pass; %d drawn by the rule; pair compared on %d: %d mismatches' % (n, mism, rule_drawn, pair_n, pair_mism))
    for t in bad[:25]:
        print('    ' + t)
    ok1 = n > 0 and mism == 0 and pair_mism == 0

    # (2) feature_diff lines, rule vs plant
    ok2 = True
    for (fr, res, *_r), (_f2, pres, *_p) in zip(base, plant):
        bt, pt = texts(res), texts(pres)
        for name, test in TOKENS:
            nb, np_ = sum(1 for t in bt if test(t)), sum(1 for t in pt if test(t))
            good = nb == 0 and np_ > 0
            ok2 = ok2 and good
            print('(2) DIFF  f%d %-26s rule %d line(s), plant %d  %s' % (fr, name, nb, np_, 'PASS' if good else 'FAIL'))
            for t in (x for x in bt if test(x)):
                print('      rule: ' + t)
        gone, new = [t for t in pt if t not in set(bt)], [t for t in bt if t not in set(pt)]
        print('    f%d: the rule removed %d report line(s), added %d:' % (fr, len(gone), len(new)))
        for t in gone[:12]:
            print('      - ' + t)
        for t in new[:12]:
            print('      + ' + t)
    for fr, res, *_r in base:
        o = _r[-1]
        for line in o['text'].splitlines():
            if 'read_rule' in line or 'd1 ' in line:
                print('    rig f%d: %s' % (fr, line.strip()[:200]))

    # (3) the crop
    if a.crop:
        box = [int(x) for x in a.crop.split(',')]
        for fr in frames:
            r = lum(find_png(os.path.join(out, 'rule_%d' % fr)), box)
            p = lum(find_png(os.path.join(out, 'old_%d' % fr)), box)
            gp = a.game_png or None
            g = lum(gp, box) if gp else None
            fmt = lambda v: '-' if v is None else '%.1f' % v
            ratio = lambda x, y: '-' if (x is None or y is None or y == 0) else '%.2f' % (x / y)
            print('(3) CROP  f%d %s: ours rule %s, ours old %s, game %s; old/game %s, rule/game %s, old/rule %s' % (fr, box, fmt(r), fmt(p), fmt(g), ratio(p, g), ratio(r, g), ratio(p, r)))
    print('%s  D1 read rule (1) %s (2) %s' % ('PASS' if ok1 and ok2 else 'FAIL', 'PASS' if ok1 else 'FAIL', 'PASS' if ok2 else 'FAIL'))
    return 0 if ok1 and ok2 else 1


if __name__ == '__main__':
    sys.exit(main())
