r"""Session 101 lane 34: the post pass proven against the game's own frames (docs\renderer\POST_PASS.md).

  py -I tools\post\post_proof.py [--root DIR] [--only NAME[,NAME]] [--fault bloom_half|dof_threshold|skip_hdr] [--out DIR] [--no-run]
  py -I tools\post\post_proof.py --selftest          (the gate check: every case within tolerance, every planted fault OUT of tolerance on the case where it shows)

Per case (a capture frame): out\renderer\dev\post_proof.exe renders the frame up to just before the post region, runs OUR post pass
(src\renderer_scene\post_pass.*) on the scene colour and depth with the parameters read from the game's post draws by name, renders the game's own post
draws for the reference, and writes ours.png / game.png (+ the intermediate pictures). This script compares them with owned_check's tolerance
(tools\native\owned_verdict.py: d = the largest channel difference per pixel; PASS = p99.9 <= 8, no 16x16 block mean > 2, share of d > 4 <= 0.3%) and writes
the sheet out\s101\post\sheet.png (game final | ours | difference, every panel labelled). Both sides are the picture BEFORE the display curve (the game
applies Bt709Encode(SrgbDecode(c)) at the present, the viewer's rdr backend in its shot read-back): like with like, said in every line.
Every result line names its reference: "vs the game's final picture, capture X frame N".
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

from PIL import ImageFile  # noqa: E402
ImageFile.LOAD_TRUNCATED_IMAGES = True  # (a case zip's picture.png can be cut short: lane 33 met it too)
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / 'tools' / 'native'))
import owned_verdict as OV  # noqa: E402  (the tolerance and block_mean_max: one definition)


def main_checkout():
    common = subprocess.run(['git', '-C', str(ROOT), 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True).stdout.strip()
    return Path(common).parent if common else ROOT


MAIN = main_checkout()
EXACT_MAX = 1  # review 0: beside owned_check's tolerance, an exact bar for the replay comparison
OUT = MAIN / 'out' / 's101' / 'post'
CASES = {  # name -> (capture, frame, what it is)
    'match': ('s100_match300', 2, 'match frame (depth of field + haze)'),
    'entrance': ('s100_s89k_a', 1, 'entrance with the HDR chain (bloom + star glare; frame 1 has them lit: bloom max 39, glare max 59 of 255; frame 5 is dark)'),
    'wm30': ('s100_wm30_300', 1, '30 Years of WrestleMania match (post + the film filter)'),
}
# A real past bug, re-found (S100 summary 2026-11-10 07:15 / 11:24: "the post chain's first passes read fetch words that are not a texture": the dof pass, the bloom
# chain and the glare read NOTHING, so the game's own blur came out black and the picture washed out). The capture brock_s15_300 frame 15 was taken before the fix and
# still carries it (11 slots of image 0). Our pass reads the real inputs, so the proof must report this frame OVER, with the blur as the first piece that differs.
KNOWN_BAD = {'old_empty_inputs': ('brock_s15_300', 15, 'entrance captured before the S100 empty-fetch fix (the game side reads nothing)')}
CASES.update(KNOWN_BAD)
# fault -> the case where it shows
FAULTS = {'bloom_half': 'entrance', 'dof_threshold': 'match', 'skip_hdr': 'entrance'}


def last_frame(capture):
    n = len(list((MAIN / 'out' / 'captures' / capture / 'frames').glob('frame_*.bin')))
    if not n:
        raise SystemExit(f'FAIL  capture {capture} has no frames (cannot read {MAIN / "out" / "captures" / capture})')
    return n - 1


def exe(root):
    p = Path(root) / 'out' / 'renderer' / 'dev' / 'post_proof.exe'
    if not p.exists():
        raise SystemExit(f'FAIL  {p} is not built: pwsh -NoProfile -File tools\\post\\build_proof.ps1')
    return str(p)


def run_case(root, name, fault='', out=None):
    cap, frame, _ = CASES[name]
    frame = last_frame(cap) if frame is None else frame
    folder = Path(out or OUT) / (name + (f'_{fault}' if fault else ''))
    folder.mkdir(parents=True, exist_ok=True)
    cmd = [exe(root), '--capture', str(MAIN / 'out' / 'captures' / cap), '--frame', str(frame), '--proof', '--out', str(folder), '--root', str(root), '--validation', '1']
    if fault:
        cmd += ['--fault', fault]
    for attempt in range(4):  # the shared GPU: retry only the frame-ring memory error
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
        text = p.stdout + p.stderr
        if p.returncode == 0:
            break
        if 'frame ring' in text and attempt < 3:
            print(f'  (GPU busy: "Not enough graphics memory for the frame ring", attempt {attempt + 1}; retrying)')
            continue
        raise SystemExit(f'FAIL  post_proof {name}{" +" + fault if fault else ""} exited {p.returncode}: {text[-600:]}')
    meta = json.loads((folder / 'result.json').read_text(encoding='utf-8'))
    if meta['validation_errors']:
        raise SystemExit(f'FAIL  {name}: {meta["validation_errors"]} validation errors')
    return folder, meta, frame


def defaults_check(root, fault=''):
    """Goal 2b: DefaultPostParams (the viewer's look settings on rdr) against the tables the game's draws carried, entrance and match. Returns (ok, text)."""
    out, ok = [], True
    for name in ('match', 'entrance'):
        cap, frame, _ = CASES[name]
        cmd = [exe(root), '--capture', str(MAIN / 'out' / 'captures' / cap), '--frame', str(frame), '--defaults', '--root', str(root)] + (['--fault', fault] if fault else [])
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
        last = [l for l in (p.stdout + p.stderr).splitlines() if 'worst table difference' in l]
        if not last:
            raise SystemExit(f'FAIL  post_proof --defaults {name} gave no result: {(p.stdout + p.stderr)[-400:]}')
        out.append(f'{name} (capture {cap} frame {frame}): ' + last[-1].replace('[post_proof] ', ''))
        ok = ok and p.returncode == 0
    return ok, '; '.join(out)


ZIPS = {  # name -> (run folder glob, what it is): Check A's case zips whose picture.png is what was on screen at the last capture frame
    'bryan_zip': ('*bryan_entrance', 'Bryan entrance (owner run)'),
    'wm30_zip': ('*s90_wm30', '30 Years match (owner run)'),
    'brock_zip': ('*175051_s100_owner_s89_brock', 'Brock ring-in (owner run)'),
}


def curve(rgb):
    """The game's display-gamma curve, Bt709Encode(SrgbDecode(c)) (viewer_hlsl.h:788, run 6814), on uint8 RGB."""
    x = rgb.astype(np.float64) / 255.0
    lin = np.where(x <= 0.04045, x / 12.92, ((x + 0.055) / 1.055) ** 2.4)
    out = np.where(lin < 0.018, 4.5 * lin, 1.099 * np.power(np.maximum(lin, 1e-12), 0.45) - 0.099)
    return np.clip(np.round(out * 255.0), 0, 255).astype(np.int16)


def picture_check(root, name, out=None, fault=''):
    """The NON-circular check (review 0). A case zip's picture.png is what was on screen (the owner renderer's present, display curve on). Our pass runs on the
    zip's last capture frame; only the pixels no later draw touched (the replay's final surface equals the post output) are compared, ours + the curve against
    picture.png, with owned_check's tolerance; the baseline is the replay's own post output + the curve against the same picture (it says whether the curve
    and the mask are right). It proves our pass + curve reproduce what the owner path put on screen; it does not prove the 360's own output (the owner path
    runs the same ported effects) and it is blind where the HUD covers the picture."""
    import zipfile
    glob, label_text = ZIPS[name]
    zs = sorted((MAIN / 'out' / 's101' / 'runs').glob(glob + '/case_*.zip'))
    if not zs:
        raise SystemExit(f'FAIL  no case zip for {glob} under {MAIN / "out" / "s101" / "runs"}')
    zpath = zs[-1]
    dest = (out or OUT) / 'zips' / name
    cap = dest / 'capture'
    if not (cap / 'frames').exists():
        z = zipfile.ZipFile(zpath)
        for m in z.namelist():
            if m.startswith('capture/') or m == 'picture.png':
                z.extract(m, dest)
    pic = dest / 'picture.png'
    if not pic.exists():
        raise SystemExit(f'FAIL  {zpath} has no picture.png')
    frames = len(list((cap / 'frames').glob('frame_*.bin')))
    folder = dest / ('proof' + (f'_{fault}' if fault else ''))
    folder.mkdir(parents=True, exist_ok=True)
    cmd = [exe(root), '--capture', str(cap), '--frame', str(frames - 1), '--proof', '--out', str(folder), '--root', str(root), '--validation', '1'] + (['--fault', fault] if fault else [])
    for attempt in range(4):
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=900)
        text = p.stdout + p.stderr
        if p.returncode == 0:
            break
        if 'frame ring' in text and attempt < 3:
            continue
        return dict(case=name, ok=False, note=f'post_proof exited {p.returncode}: {text[-300:]}')
    P = np.asarray(Image.open(pic).convert('RGB')).astype(np.int16)
    E = np.asarray(Image.open(folder / 'end.png').convert('RGB')).astype(np.int16)
    G = np.asarray(Image.open(folder / 'game.png').convert('RGB')).astype(np.int16)
    O = np.asarray(Image.open(folder / 'ours.png').convert('RGB')).astype(np.int16)
    if not (P.shape == E.shape == G.shape == O.shape):  # an owner run at 1440p: its picture is a 2x render, not comparable pixel by pixel with the 720p replay
        return dict(case=name, ok=None, note=f'not comparable: picture {P.shape[1]}x{P.shape[0]} (the owner ran at a render scale), replay {E.shape[1]}x{E.shape[0]}')
    mask = (np.abs(E - G).max(axis=2) == 0)  # pixels no draw after the post chain changed
    cov = float(mask.mean())

    def judge(img):
        d = np.abs(curve(img.astype(np.uint8)) - P).max(axis=2)
        dm = np.where(mask, d, 0)
        vals = d[mask]
        if vals.size == 0:
            return dict(over=['no unmasked pixels'], p999=0, block=0, share=0, mean=0, max=0)
        n = numbers(dm)
        n['p999'] = round(float(np.percentile(vals, 99.9)), 2)  # over the compared pixels only
        n['share'] = round(float((vals > OV.TOL_SMALL).mean()), 5)
        n['mean'] = round(float(vals.mean()), 3)
        n['over'] = [o for o in n['over'] if not o.startswith('max d')]  # (the exact bar is for the replay comparison)
        if n['p999'] > OV.TOL_P999:
            n['over'].append(f'p99.9 {n["p999"]} > {OV.TOL_P999:g}')
        if n['share'] > OV.TOL_SHARE:
            n['over'].append(f'share>{OV.TOL_SMALL} {100 * n["share"]:.2f}% > {100 * OV.TOL_SHARE:.1f}%')
        n['over'] = sorted(set(n['over']))
        return n

    base, ours = judge(G), judge(O)
    # The picture is a later instant than the capture's last frame (the moving wrestler differs by a few levels in the baseline too), so the absolute p99.9 / block rules
    # are not the bar here: ours must not be worse than the baseline (p99.9 + 1 level, share + 0.1%) and must keep owned_check's share rule (<= 0.3% of pixels over 4).
    worse = []
    if ours['p999'] > base['p999'] + 1:
        worse.append(f"p99.9 {ours['p999']} > baseline {base['p999']} + 1")
    if ours['share'] > base['share'] + 0.001:
        worse.append(f"share {100 * ours['share']:.2f}% > baseline {100 * base['share']:.2f}% + 0.1%")
    if ours['share'] > OV.TOL_SHARE:
        worse.append(f"share>{OV.TOL_SMALL} {100 * ours['share']:.2f}% > {100 * OV.TOL_SHARE:.1f}%")
    ours['over'] = worse
    return dict(case=name, ok=not worse, base=base, ours=ours, coverage=round(cov, 3), zip=zpath.name, frame=frames - 1, what=label_text,
                ref=f"vs picture.png of {zpath.name} (what was on screen: the owner renderer's present, display curve on), unmasked pixels only")


def picture_line(r):
    if 'note' in r:
        return f"{r['case']}: {r['note']}"
    o, b = r['ours'], r['base']
    tag = 'WITHIN TOLERANCE' if r['ok'] else 'OVER: ' + '; '.join(o['over'])
    return (f"{r['case']}: {tag}  our pass + display curve {r['ref']} (capture frame {r['frame']}, {100 * r['coverage']:.0f}% of the picture compared): mean d {o['mean']}, p99.9 {o['p999']}, "
            f"share>{OV.TOL_SMALL} {100 * o['share']:.2f}%; baseline (the replay's own post output + curve vs the same picture): p99.9 {b['p999']}, share {100 * b['share']:.2f}%")


def diff(a, b):
    ia = np.asarray(Image.open(a).convert('RGB')).astype(np.int16)
    ib = np.asarray(Image.open(b).convert('RGB')).astype(np.int16)
    if ia.shape != ib.shape:
        raise SystemExit(f'FAIL  picture sizes differ: {a} {ia.shape} vs {b} {ib.shape}')
    return np.abs(ia - ib).max(axis=2)


def numbers(d):
    p999 = float(np.percentile(d, 99.9))
    blk = OV.block_mean_max(d)
    share = float((d > OV.TOL_SMALL).mean())
    over = []
    if int(d.max()) > EXACT_MAX:  # the exact bar: the replay is deterministic, so it expects zeros (a rounding level of 1 is the most it forgives)
        over.append(f'max d {int(d.max())} > {EXACT_MAX} (exact bar)')
    if p999 > OV.TOL_P999:
        over.append(f'p99.9 {p999:.1f} > {OV.TOL_P999:g}')
    if blk > OV.TOL_BLOCK:
        over.append(f'block mean {blk:.2f} > {OV.TOL_BLOCK:g}')
    if share > OV.TOL_SHARE:
        over.append(f'share>{OV.TOL_SMALL} {100 * share:.2f}% > {100 * OV.TOL_SHARE:.1f}%')
    return dict(mean=round(float(d.mean()), 3), p999=round(p999, 2), block=round(blk, 2), share=round(share, 5), max=int(d.max()), over=over)


def heat(d):
    h = np.zeros(d.shape + (3,), np.uint8)
    h[(d >= 1) & (d <= OV.TOL_SMALL)] = (40, 70, 200)
    h[(d > OV.TOL_SMALL) & (d <= OV.TOL_P999)] = (255, 170, 30)
    h[d > OV.TOL_P999] = (255, 30, 30)
    return Image.fromarray(h)


def label(im, text):
    im = im.copy().convert('RGB')
    d = ImageDraw.Draw(im)
    d.rectangle([0, 0, im.width, 14], fill=(0, 0, 0))
    d.text((4, 2), text, fill=(255, 255, 255))
    return im


def judge_case(root, name, fault='', out=None):
    folder, meta, frame = run_case(root, name, fault, out)
    ref = f"vs the capture replayed through our ported post effects (same SPIR-V, constants read from its draws: partly circular), capture {CASES[name][0]} frame {frame}"
    res = dict(case=name, capture=CASES[name][0], frame=frame, fault=fault, ref=ref, passes=meta['passes'], final=numbers(diff(folder / 'ours.png', folder / 'game.png')), parts={})
    for part in ('blur', 'bloom', 'glare'):
        if (folder / f'ours_{part}.png').exists() and (folder / f'game_{part}.png').exists():
            res['parts'][part] = numbers(diff(folder / f'ours_{part}.png', folder / f'game_{part}.png'))
    res['pass'] = not res['final']['over']
    res['folder'] = str(folder)
    return res


def line(r):
    f = r['final']
    tag = 'WITHIN TOLERANCE' if r['pass'] else 'OVER: ' + '; '.join(f['over'])
    parts = ''.join(f"  [{k}: p99.9 {v['p999']} share {100 * v['share']:.2f}%]" for k, v in r['parts'].items())
    return (f"{r['case']}{' +' + r['fault'] if r['fault'] else ''}: {tag}  ({r['passes']} passes; mean d {f['mean']}, p99.9 {f['p999']}, block {f['block']}, share>{OV.TOL_SMALL} "
            f"{100 * f['share']:.2f}%, max {f['max']}) {r['ref']}; both sides before the display curve{parts}")


def sheet(results, path):
    rows = []
    for r in results:
        fo = Path(r['folder'])
        g, o = Image.open(fo / 'game.png').convert('RGB'), Image.open(fo / 'ours.png').convert('RGB')
        d = heat(diff(fo / 'ours.png', fo / 'game.png'))
        rows.append([label(g, f"REPLAY of the capture's own post draws ({r['capture']} frame {r['frame']}, before display curve)"),
                     label(o, f"OURS: post_pass on the game's pre-post colour + depth{' +PLANTED ' + r['fault'] if r['fault'] else ''}"),
                     label(d, f"DIFFERENCE (blue 1-4, orange 5-8, red >8): p99.9 {r['final']['p999']} share {100 * r['final']['share']:.2f}%")])
    w, h = rows[0][0].size
    w2, h2 = w // 2, h // 2
    s = Image.new('RGB', (w2 * 3, h2 * len(rows)), (20, 20, 20))
    for j, row in enumerate(rows):
        for i, p in enumerate(row):
            s.paste(p.resize((w2, h2), Image.LANCZOS), (i * w2, j * h2))
    path.parent.mkdir(parents=True, exist_ok=True)
    s.save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=str(ROOT))
    ap.add_argument('--only', default='')
    ap.add_argument('--fault', default='')
    ap.add_argument('--out', default='')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    names = [n for n in (a.only.split(',') if a.only else [c for c in CASES if c not in KNOWN_BAD])]
    out = Path(a.out) if a.out else None
    if a.selftest:
        results, bad = [], []
        for n in names:
            r = judge_case(a.root, n, '', out)
            results.append(r)
            print(('PASS  ' if r['pass'] else 'FAIL  ') + line(r))
            if not r['pass']:
                bad.append(n)
        for fault, case in FAULTS.items():
            if case not in names:
                continue
            r = judge_case(a.root, case, fault, out)
            ok = not r['pass']  # the planted fault must be OUT of tolerance
            print(('PASS  ' if ok else 'FAIL  ') + f'planted {fault} must fail the bar: ' + line(r))
            if not ok:
                bad.append('fault:' + fault)
        # The non-circular check: what was on screen (Check A's case zips' picture.png). A zip whose owner run drew at a render scale is reported, not compared.
        for zn in ZIPS:
            r = picture_check(a.root, zn, out)
            if r.get('ok') is None:
                print('NOTE  ' + picture_line(r))
                continue
            print(('PASS  ' if r['ok'] else 'FAIL  ') + picture_line(r))
            if not r['ok']:
                bad.append('picture:' + zn)
            for fault in ('dof_threshold', 'skip_hdr'):  # the faults that show on this picture (bloom_half does not: Brock's frame holds almost no bloom)
                rf = picture_check(a.root, zn, out, fault)
                okf = rf['ok'] is False
                print(('PASS  ' if okf else 'FAIL  ') + f'planted {fault} must fail against the real picture: ' + picture_line(rf)[:360])
                if not okf:
                    bad.append(f'picture:{zn}:{fault}')
        ok, text = defaults_check(a.root)
        print(('PASS  ' if ok else 'FAIL  ') + 'viewer defaults (DefaultPostParams) vs the game tables: ' + text)
        if not ok:
            bad.append('defaults')
        okf, textf = defaults_check(a.root, 'defaults_weight')
        print(('PASS  ' if not okf else 'FAIL  ') + 'planted defaults_weight (one hole-fill weight 1% off) must be caught: ' + textf)
        if okf:
            bad.append('fault:defaults_weight')
        r = judge_case(a.root, 'old_empty_inputs', '', out)  # the real past bug: must be OVER, and its blur piece (the dof pass) must differ too
        ok = (not r['pass']) and bool(r['parts'].get('blur', {}).get('over'))
        print(('PASS  ' if ok else 'FAIL  ') + 'real past bug re-found (S100: post inputs read nothing, the game-side blur black): ' + line(r))
        if not ok:
            bad.append('known-bad:old_empty_inputs')
        sheet(results + [r], (out or OUT) / 'sheet.png')
        print('sheet ->', (out or OUT) / 'sheet.png')
        if bad:
            print('FAIL  post proof:', ', '.join(bad))
            return 1
        print('PASS  post proof: all cases within tolerance, all planted faults caught')
        return 0
    results = [judge_case(a.root, n, a.fault, out) for n in names]
    for r in results:
        print(line(r))
    sheet(results, (out or OUT) / 'sheet.png')
    print('sheet ->', (out or OUT) / 'sheet.png')
    (((out or OUT) / 'results.json')).write_text(json.dumps(results, indent=1), encoding='utf-8')
    return 0 if all(r['pass'] for r in results) else 1


if __name__ == '__main__':
    sys.exit(main())
