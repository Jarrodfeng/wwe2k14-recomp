r"""The judge, v1 (Session 101 lane 20, RENDERER_ARC.md section 4 tool 3): two arms of the same frames, differences masked by class, put on a sheet.
"A number points at a place; eyes give the verdict." Docs: docs\renderer\JUDGE.md.

  py -I tools\judge\judge.py CAPTURE_DIR_OR_CASE_ZIP --arm-a SPEC --arm-b SPEC [--frames 1,5 | last] [--out DIR] [--threshold 12] [--min-px 40]

An arm is "a thing that produces a picture per frame for a capture" (the seam the scene arm of Phase C plugs into). SPEC:
  fview                       the capture through fview (the real binder + renderer), the effect descriptions of this checkout
  fview:fx=DIR                the same with the effect descriptions of DIR planted (every effect whose files differ from this checkout's is rebuilt
                              and bound, fview --apply-reload: an alpha_test flag, a constant layout-neutral edit of a .frag / .vert, ...)
  fview:exe=PATH[:fx=DIR]     another fview.exe (another build)
  rplay:EXE                   a picture from another rplay build (no draw names: its regions say "unattributed")
  picture:PATH                a reference picture (one frame only)
Arms made by fview carry names: for every sampled pixel the chain of draws behind it (fview --judge-out), so a region is named by class / origin / effect.

Output (under the main checkout's out\s101\judge\<name>_<time>\; game pictures: local only): judge.json, judge_sheet.png.
"""
import argparse
import collections
import json
import os
import re
import shutil
import subprocess
import sys
import time
import zipfile
from pathlib import Path

import warnings

warnings.filterwarnings('ignore', message='A NumPy version')
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from scipy import ndimage

HERE = Path(__file__).resolve().parents[2]


def main_checkout() -> Path:
    env = os.environ.get('RDR_MAIN_CHECKOUT')
    if env:
        return Path(env)
    try:
        common = subprocess.run(['git', '-C', str(HERE), 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True,
                                timeout=20).stdout.strip()
        if common:
            return Path(common).parent
    except Exception:
        pass
    return HERE


MAIN = main_checkout()
BASE_FX = HERE / 'src' / 'renderer' / 'shaders' / 'fx'

# ---------------------------------------------------------------- the class map (effect name -> class)
POST = {'yPostBase', 'yToneMap', 'yGamma', 'yHalation', 'yDefocus', 'yDownScale', 'yScaleDownFB', 'yScaleDownZ', 'yComposition', 'yAntialiasing',
        'yMonotone', 'yRetroFilm', 'yCopyFrBuffer', 'yCombineTex', 'yPartsBlur', 'yEmboss', 'yVelocity', 'yGlareEtc', 'yBlur', 'yRadialBlur', 'yDebugPrimitive'}
CLASS_RULES = [
    (r'^y(Aud|FlashDefault$)', 'crowd'), (r'^yCh_', 'wrestler'), (r'^yBG_', 'arena'), (r'^yProp_', 'prop'), (r'^y(ShadowMap|PlaneShadowMap|CircleShadow)', 'shadow'),
    (r'^y(Particle|SoftParticle|AfterImage)', 'particle'), (r'^y(Sprite|Rect)$', 'ui2d'), (r'^yDefault$', 'model'), (r'^yMovieDefault$', 'video'),
    (r'^yLiquidMap', 'liquid'), (r'^y(PreLight|RimLightMap|ZPass)', 'scene_prep'),
]


def effect_class(effect: str) -> str:
    if not effect:
        return 'unknown'
    if effect in POST:
        return 'post'
    for pat, name in CLASS_RULES:
        if re.match(pat, effect):
            return name
    return 'other'


# ---------------------------------------------------------------- input
def resolve_capture(path: Path, work: Path) -> Path:
    """A capture folder as it is; a case zip: its capture\\ folder unpacked (once) under work\\."""
    if path.is_dir():
        if not (path / 'images.bin').exists():
            sys.exit(f'FAIL {path} is a folder without images.bin: not a capture')
        return path
    if path.suffix.lower() != '.zip' or not path.exists():
        sys.exit(f'FAIL {path}: not a capture folder or a case zip')
    dest = work / path.stem
    st = path.stat()
    stamp = f'{st.st_size} {int(st.st_mtime)}'
    stamp_file = dest / 'STAMP'
    # The stamp is written LAST (after the rename): a half-finished unpack, a changed zip or a missing folder is extracted again.
    if not (stamp_file.exists() and stamp_file.read_text() == stamp and (dest / 'capture' / 'images.bin').exists()):
        tmp = work / (path.stem + '.tmp')
        for old in (tmp, dest):
            if old.exists():
                shutil.rmtree(old)
        with zipfile.ZipFile(path) as z:
            names = [n for n in z.namelist() if n.startswith('capture/') and not n.endswith('/')]
            if not names:
                sys.exit(f'FAIL {path}: no capture\\ folder in the zip')
            z.extractall(tmp, members=names)
        tmp.rename(dest)
        stamp_file.write_text(stamp)
    return dest / 'capture'


def frame_count(cap: Path) -> int:
    return len(list((cap / 'frames').glob('frame_*.bin')))


# ---------------------------------------------------------------- arms
class Arm:
    def __init__(self, spec: str, label: str):
        self.spec, self.label = spec, label
        self.kind = 'fview'
        self.exe = HERE / 'out' / 'renderer' / 'dev' / 'fview.exe'
        self.fx = None
        self.edited = []
        self.ref = None
        parts = spec.split(':', 1)
        head = parts[0]
        rest = parts[1] if len(parts) > 1 else ''
        if head == 'fview':
            # key=value options, separated by ':' but a Windows drive letter's colon is part of the value: split on ':key=' boundaries
            for m in re.finditer(r'(?:^|:)(exe|fx)=(.*?)(?=:(?:exe|fx)=|$)', rest):
                if m.group(1) == 'exe':
                    self.exe = Path(m.group(2))
                else:
                    self.fx = Path(m.group(2))
            if rest and not re.search(r'(exe|fx)=', rest):
                sys.exit(f'FAIL arm {spec!r}: fview options are exe=PATH and fx=DIR')
        elif head == 'rplay':
            self.kind, self.exe = 'rplay', Path(rest)
        elif head == 'picture':
            self.kind, self.ref = 'picture', Path(rest)
        else:
            sys.exit(f'FAIL arm {spec!r}: unknown kind (fview, rplay:EXE, picture:PATH)')
        if self.kind == 'fview' and not re.search(r'(?:^|:)exe=', rest):
            sys.path.insert(0, str(HERE / 'tools' / 'fview'))   # lane 218: the default exe goes through the shared locator (refuses a stale build); fview:exe=PATH is an explicit choice (an A/B of builds) and is not checked
            import fview_exe as fe
            self.exe = Path(fe.find_or_exit(root=HERE, kind='dev'))
        if self.kind in ('fview', 'rplay') and not self.exe.exists():
            sys.exit(f'FAIL arm {spec!r}: {self.exe} does not exist (build it: tools\\fview\\build.ps1 / tools\\renderer\\build.ps1)')
        if self.kind == 'picture' and not self.ref.exists():
            sys.exit(f'FAIL arm {spec!r}: {self.ref} does not exist')
        if self.fx is not None:
            self.edited = edited_effects(self.fx)

    @property
    def named(self):
        return self.kind == 'fview'


def edited_effects(fx: Path) -> list:
    """The effects whose description folder files differ from this checkout's."""
    if not fx.is_dir():
        sys.exit(f'FAIL fx dir {fx} is not a folder')
    out = set()
    for f in sorted(fx.iterdir()):
        if not f.is_file():
            continue
        base = BASE_FX / f.name
        if base.exists() and base.read_bytes() == f.read_bytes():
            continue
        if f.suffix == '.glsl':
            sys.exit(f'FAIL {f.name} (a shared glsl) differs: fview reloads one effect at a time; the judge v1 plants per-effect files only')
        out.add(f.name.split('.')[0])
    for f in BASE_FX.iterdir():  # a file the planted copy lacks is also a difference
        if f.is_file() and not (fx / f.name).exists():
            sys.exit(f'FAIL {fx} lacks {f.name}: a planted folder is a full copy of the effect folder')
    return sorted(out)


def run(cmd, timeout, what):
    t0 = time.time()
    for attempt in range(4):  # other lanes share the GPU: "Not enough graphics memory" is transient, so wait and ask again (named, never silent)
        p = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, timeout=timeout, cwd=HERE)
        if p.returncode == 0 or 'Not enough graphics memory' not in (p.stdout + p.stderr):
            break
        print(f'  ({what}: the GPU is busy, waiting 20 s, try {attempt + 2} of 4)', flush=True)
        time.sleep(20)
    if p.returncode != 0:
        tail = '\n'.join((p.stdout + p.stderr).strip().splitlines()[-8:])
        sys.exit(f'FAIL {what}: exit {p.returncode}\n{tail}')
    return time.time() - t0, p.stdout


def render_arm(arm: Arm, cap: Path, frame: int, work: Path, pixels: list | None, tag: str):
    """-> (picture RGB uint8 array, chains dict {(x, y): [nodes]} or None, seconds). `pixels` None: the picture only."""
    d = work / f'{tag}_f{frame}'
    d.mkdir(parents=True, exist_ok=True)
    if arm.kind == 'picture':
        return np.asarray(Image.open(arm.ref).convert('RGB')), None, 0.0
    if arm.kind == 'rplay':
        png = d / 'picture.png'
        sec, _ = run([arm.exe, '--capture', cap, '--frames', frame + 1, '--out', png], 600, f'rplay arm {arm.spec}')
        if not png.exists():
            sys.exit(f'FAIL rplay arm {arm.spec}: no picture written')
        return np.asarray(Image.open(png).convert('RGB')), None, sec
    cmd = [arm.exe, '--capture', cap, '--frame', frame, '--judge-out', d, '--root', HERE]
    if arm.fx is not None:
        cmd += ['--fx-dir', arm.fx]
        if arm.edited:
            cmd += ['--apply-reload', ','.join(arm.edited)]
    if pixels:
        pf = d / 'pixels.txt'
        pf.write_text('\n'.join(f'{x} {y}' for x, y in pixels) + '\n')
        cmd += ['--judge-pixels', pf]
    sec, out = run(cmd, 1500, f'fview arm {arm.spec}')
    for e in arm.edited:  # a plant that applies nothing must not pass (review 1i): every edited effect has to have changed a variant
        m = re.search(r'applied reload of ' + re.escape(e) + r': (\d+) variants, (\d+) changed, (\d+) skipped', out)
        if not m:
            sys.exit(f'FAIL fview arm {arm.spec}: no "applied reload of {e}" line (the plant was not applied)')
        if int(m.group(2)) == 0:
            sys.exit(f'FAIL fview arm {arm.spec}: the edit of {e} changed 0 of {m.group(1)} variants ({m.group(3)} skipped): the plant applies nothing')
    png = d / 'picture.png'
    if not png.exists():
        sys.exit(f'FAIL fview arm {arm.spec}: no picture written ({png})')
    chains = None
    if pixels:
        pj = d / 'pixels.json'
        if not pj.exists():
            sys.exit(f'FAIL fview arm {arm.spec}: no pixels.json')
        chains = {(p['x'], p['y']): p['chain'] for p in json.loads(pj.read_text(encoding='utf-8'))['pixels']}
    return np.asarray(Image.open(png).convert('RGB')), chains, sec



# ---------------------------------------------------------------- the difference
def find_regions(a, b, threshold, min_px, company):
    """Per-pixel max channel difference. Two fview arms are deterministic: every difference is real, so the threshold is near zero and no pixel needs company.
    rplay / picture arms (other builds, other drivers) keep look-right-not-exact: >= threshold of 255 and at least 4 counting pixels in the 5x5 neighbourhood.
    Counting pixels within 6 px of each other form one region; regions under min_px are dropped (and counted in `stats`: nothing is hidden silently)."""
    d = np.abs(a.astype(np.int16) - b.astype(np.int16)).max(axis=2)
    raw = d >= threshold
    mask = raw
    if company:
        mask = raw & (ndimage.uniform_filter(raw.astype(np.float32), 5) * 25 >= 4)
    stats = {'changed_pixels': int(raw.sum()), 'counted_pixels': int(mask.sum()), 'max_diff': round(float(d.max()) / 255, 4), 'dropped_regions': 0, 'dropped_region_pixels': 0}
    grouped = ndimage.binary_dilation(mask, iterations=6)
    labels, n = ndimage.label(grouped)
    regions = []
    for i in range(1, n + 1):
        m = (labels == i) & mask
        px = int(m.sum())
        if px < min_px:
            stats['dropped_regions'] += 1
            stats['dropped_region_pixels'] += px
            continue
        ys, xs = np.nonzero(m)
        vals = d[m]
        regions.append({'bbox': [int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1], 'pixels': px, 'mean_diff': round(float(vals.mean()) / 255, 4),
                        'max_diff': round(float(vals.max()) / 255, 4), '_xs': xs, '_ys': ys, '_vals': vals})
    regions.sort(key=lambda r: -r['pixels'] * r['mean_diff'])
    return regions, d, stats


def sample_pixels(region, count=3):
    xs, ys, vals = region['_xs'], region['_ys'], region['_vals']
    order = np.argsort(xs.astype(np.int64) * 100000 + ys)
    picks = {int(np.argmax(vals))}
    for k in range(1, count):
        picks.add(int(order[int(len(order) * k / count)]))
    return [(int(xs[i]), int(ys[i])) for i in sorted(picks)]


def content_node(chain):
    """The last non-post draw that wrote the pixel (highest op: UI over scene, nearer over farther); else the last post draw; None for a chain with no draw."""
    draws = [n for n in chain if n.get('kind') == 'draw' and n.get('effect') is not None]
    content = [n for n in draws if effect_class(n['effect']) != 'post']
    pool = content or draws
    return max(pool, key=lambda n: n['op']) if pool else None


def draws_of(chain):
    return collections.Counter(f"{n.get('effect', '')}|{n.get('ps_variant', '')}" for n in chain if n.get('kind') == 'draw')


def hist_maps(chain):
    """{(surface, through): {(op, effect, raw value)}}: what changed the pixel on each walked surface (judge.cpp 'hist' nodes)."""
    out = {}
    for n in chain:
        if n.get('kind') == 'hist':
            out[(n['surface'], n['through'])] = {(w[0], w[1], w[2]) for w in n['writes']}
    return out


def earliest_difference(chains_a, chains_b, samples):
    """The EARLIEST op at which the arms differ at a sampled pixel (ops align: the same capture): an op that changed the pixel in one arm and not the other, or to a
    different value. Later passes (post, copies) only carry that difference on, so the earliest drawing op is the cause. -> (op, effect) or None."""
    best = None
    for s in samples:
        ha, hb = hist_maps(chains_a[s]), hist_maps(chains_b[s])
        for key in set(ha) & set(hb):
            for item in ha[key] ^ hb[key]:
                op, effect = item[0], item[1]
                if len(effect) == 1:  # a copy / clear: carries a difference, does not make one
                    continue
                if best is None or op < best[0]:
                    best = (op, effect)
    return best


def attribute(region, chains_a, chains_b, arm_a, arm_b):
    samples = sample_pixels(region)
    info = {'samples': [list(s) for s in samples]}
    if not (arm_a.named and arm_b.named and chains_a is not None and chains_b is not None):
        info.update({'class': 'unattributed', 'basis': 'an arm without names (rplay / picture)', 'effect_a': None, 'effect_b': None, 'origin': None, 'changed_effects': []})
        return info
    per = {}
    for tag, chains in (('a', chains_a), ('b', chains_b)):
        nodes = [content_node(chains[s]) for s in samples]
        per[tag] = [n for n in nodes if n]

    def top(nodes):
        return collections.Counter(n['effect'] for n in nodes).most_common(1)[0][0] if nodes else None

    ea, eb = top(per['a']), top(per['b'])
    info['effect_a'], info['effect_b'] = ea, eb
    info['class_a'], info['class_b'] = (effect_class(ea) if ea else 'none'), (effect_class(eb) if eb else 'none')
    info['visible_class'] = info['class_b'] if eb else info['class_a']  # what is visible in arm B: the content node, never the cause
    only = {}
    for tag, chains in (('a', chains_a), ('b', chains_b)):
        c = collections.Counter()
        for s in samples:
            c.update(draws_of(chains[s]))
        only[tag] = c
    changed = sorted(k for k in (set(only['a']) | set(only['b'])) if only['a'][k] != only['b'][k])
    changed_effects = sorted({k.split('|')[0] for k in changed if k.split('|')[0]})
    info['changed_effects'] = changed_effects
    writers = {n['effect'] for chains in (chains_a, chains_b) for s in samples for n in chains[s] if n.get('kind') == 'draw' and n.get('effect')}
    info['unmapped_effects'] = sorted(e for e in writers if effect_class(e) in ('other', 'unknown'))
    first = earliest_difference(chains_a, chains_b, samples)
    cause = None
    if first:
        cause = first[1]
        info['cause_op'] = first[0]
        info['basis'] = f'the arms first differ at op {first[0]} ({cause}) at the sampled pixels'
    elif changed_effects:
        content_changed = [e for e in changed_effects if effect_class(e) != 'post']
        cause = (content_changed or changed_effects)[0]
        info['basis'] = 'the writers reaching these pixels differ between the arms (no op-level value difference was found)'
    if cause:
        info['class'], info['cause_effect'] = effect_class(cause), cause
        info['changed_effect'] = cause
        for chains in (chains_b, chains_a):  # the origin of the draw itself
            hit = next((n for s in samples for n in chains[s] if n.get('kind') == 'draw' and n.get('effect') == cause and n.get('origin') and (not first or n['op'] == first[0])), None)
            if hit:
                info['origin'] = hit['origin']
                break
    elif eb:
        info['class'], info['basis'] = effect_class(eb), 'same writers and values in both arms at the sampled pixels; the last content draw is named'
    else:
        info['class'], info['basis'] = 'unattributed', 'no draw wrote these pixels in the sampled chains'
    info.setdefault('origin', next((n.get('origin') for n in (per['b'] or per['a'])), None))
    edited = sorted(set(arm_a.edited) | set(arm_b.edited))
    if edited:
        info['edited_effects_here'] = sorted(set(edited) & writers)
        info['cause_is_edited'] = bool(cause and cause in edited)
    return info


# ---------------------------------------------------------------- the sheet
def font(size):
    for name in ('arial.ttf', 'segoeui.ttf', 'DejaVuSans.ttf'):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default()


COLORS = [(255, 80, 80), (255, 200, 40), (80, 220, 255), (120, 255, 120), (255, 120, 255), (255, 160, 80), (160, 160, 255), (200, 255, 200)]


def sheet(rows, path, labels):
    pw, ph = 640, 360
    f, fs = font(16), font(13)
    row_h = ph + 28
    imgs = []
    for fr in rows:
        regions = fr['regions']
        text_h = 18 * (len(regions) + 2) + 6
        im = Image.new('RGB', (3 * pw + 20, row_h + text_h), (18, 18, 18))
        dr = ImageDraw.Draw(im)
        h0, w0 = fr['a'].shape[:2]
        sx, sy = pw / w0, ph / h0
        heat = np.zeros_like(fr['a'])
        base = (fr['a'].astype(np.float32) * 0.35).astype(np.uint8)
        heat[...] = base
        big = np.clip(fr['diff'].astype(np.float32) * 4, 0, 255).astype(np.uint8)
        heat[..., 0] = np.maximum(base[..., 0], big)
        panels = [(fr['a'], f"A: {labels[0]}"), (fr['b'], f"B: {labels[1]}"), (heat, 'difference (x4, red) over A')]
        for i, (arr, title) in enumerate(panels):
            x0 = i * (pw + 10)
            im.paste(Image.fromarray(arr).resize((pw, ph)), (x0, 26))
            dr.text((x0 + 2, 4), f"frame {fr['frame']}  {title}"[:80], fill=(235, 235, 235), font=f)
            for k, r in enumerate(regions):
                c = COLORS[k % len(COLORS)]
                bx = r['bbox']
                dr.rectangle([x0 + bx[0] * sx, 26 + bx[1] * sy, x0 + bx[2] * sx, 26 + bx[3] * sy], outline=c, width=2)
                if i == 2:
                    dr.text((x0 + bx[0] * sx + 3, 26 + bx[1] * sy + 2), f"R{k + 1} {r['class']}", fill=c, font=fs)
        y = row_h
        st = fr['stats']
        dr.text((4, y), f"frame {fr['frame']}: {fr['total']} region(s), {len(regions)} named; {st['changed_pixels']} px differ, max {st['max_diff']:.3f}; dropped {st['dropped_regions']} small region(s) ({st['dropped_region_pixels']} px)"
                + ('; identical within the threshold' if not st['changed_pixels'] else ''), fill=(235, 235, 235), font=f)
        for k, r in enumerate(regions):
            y += 18
            c = COLORS[k % len(COLORS)]
            eff = r.get('cause_effect') or r.get('changed_effect') or r.get('effect_b') or '-'
            dr.text((4, y + 2), f"R{k + 1}  {r['class']}  effect {eff}  seen as {r.get('visible_class', '-')}  origin {r.get('origin')}  {r['pixels']} px  mean {r['mean_diff']:.3f}  max {r['max_diff']:.3f}  [{r['basis']}]", fill=c, font=fs)
        imgs.append(im)
    total_h = sum(i.height for i in imgs)
    out = Image.new('RGB', (imgs[0].width, total_h), (18, 18, 18))
    y = 0
    for im in imgs:
        out.paste(im, (0, y))
        y += im.height
    out.save(path)


# ---------------------------------------------------------------- main
def judge(capture: Path, arm_a: Arm, arm_b: Arm, frames: list, out_dir: Path, threshold: int | None, min_px: int | None, max_regions: int = 8) -> dict:
    # Two fview arms are deterministic: near-zero threshold, no company rule (review 3). Other builds / pictures keep look-right-not-exact (12 / 40, with company).
    exact = arm_a.kind == 'fview' and arm_b.kind == 'fview'
    company = not exact
    threshold = threshold if threshold is not None else (1 if exact else 12)
    min_px = min_px if min_px is not None else (4 if exact else 40)
    reload_dir = HERE / 'out' / 'fview_reload'
    reload_before = set(p.name for p in reload_dir.iterdir()) if reload_dir.exists() else set()
    out_dir.mkdir(parents=True, exist_ok=True)
    work = out_dir / 'work'
    work.mkdir(exist_ok=True)
    rows, report_frames = [], []
    for frame in frames:
        t0 = time.time()
        pa, _, sa = render_arm(arm_a, capture, frame, work, None, 'a')
        pb, _, sb = render_arm(arm_b, capture, frame, work, None, 'b')
        if pa.shape != pb.shape:
            sys.exit(f'FAIL frame {frame}: the arms drew different sizes {pa.shape[:2]} vs {pb.shape[:2]}')
        regions, diff, stats = find_regions(pa, pb, threshold, min_px, company)
        shown = regions[:max_regions]
        chains_a = chains_b = None
        sq = 0.0
        if shown and arm_a.named and arm_b.named:
            pix = []
            for r in shown:
                pix += sample_pixels(r)
            _, chains_a, s1 = render_arm(arm_a, capture, frame, work, pix, 'a')
            _, chains_b, s2 = render_arm(arm_b, capture, frame, work, pix, 'b')
            sq = s1 + s2
        out_regions = []
        for k, r in enumerate(shown):
            info = attribute(r, chains_a, chains_b, arm_a, arm_b)
            rec = {'id': k + 1, 'bbox': r['bbox'], 'pixels': r['pixels'], 'mean_diff': r['mean_diff'], 'max_diff': r['max_diff']}
            rec.update(info)
            out_regions.append(rec)
        sec = time.time() - t0
        report_frames.append({'frame': frame, 'seconds': round(sec, 1), 'seconds_pictures': round(sa + sb, 1), 'seconds_names': round(sq, 1),
                              'regions_total': len(regions), 'regions_not_named': max(0, len(regions) - len(shown)), **stats, 'regions': out_regions})
        rows.append({'frame': frame, 'a': pa, 'b': pb, 'diff': diff, 'regions': out_regions, 'stats': stats, 'total': len(regions)})
        print(f"frame {frame}: {len(regions)} region(s), {stats['changed_pixels']} changed px, max diff {stats['max_diff']:.3f}, dropped {stats['dropped_regions']} small region(s) ({sec:.1f} s)")
        for r in out_regions:
            print(f"  R{r['id']} {r['class']:10} {r['pixels']:7} px  mean {r['mean_diff']:.3f}  effect {r.get('cause_effect') or r.get('changed_effect') or r.get('effect_b')}  [{r['basis']}]")
    unmapped = sorted({e for f in report_frames for r in f['regions'] for e in r.get('unmapped_effects', [])})
    if unmapped:
        print(f'WARN effects with no class (class "other" / "unknown"): {unmapped}: add them to CLASS_RULES / POST in tools\\judge\\judge.py')
    if reload_dir.exists():  # the reload builds this run made (each fview process with an edit writes one folder)
        for p in reload_dir.iterdir():
            if p.name not in reload_before:
                shutil.rmtree(p, ignore_errors=True)
    report = {'tool': 'judge v1 (Session 101 lane 20)', 'capture': str(capture), 'arm_a': arm_a.spec, 'arm_b': arm_b.spec, 'threshold': threshold, 'min_px': min_px,
              'company_rule': company, 'unmapped_effects': unmapped,
              'edited_effects_a': arm_a.edited, 'edited_effects_b': arm_b.edited, 'frames': report_frames,
              'seconds_per_frame': round(sum(f['seconds'] for f in report_frames) / max(1, len(report_frames)), 1)}
    (out_dir / 'judge.json').write_text(json.dumps(report, indent=1), encoding='utf-8')
    sheet(rows, out_dir / 'judge_sheet.png', (arm_a.spec, arm_b.spec))
    print(f'judge: {out_dir / "judge.json"}  sheet: {out_dir / "judge_sheet.png"}  {report["seconds_per_frame"]} s per frame')
    return report


def parse_frames(spec: str, cap: Path) -> list:
    n = frame_count(cap)
    if n == 0:
        sys.exit(f'FAIL {cap} has no frames\\frame_*.bin')
    if spec == 'last':
        return [n - 1]
    frames = [int(x) for x in spec.split(',') if x]
    bad = [f for f in frames if not 0 <= f < n]
    if bad or not frames:
        sys.exit(f'FAIL frames {spec!r}: the capture has {n} frames (0..{n - 1})')
    return frames


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('input')
    ap.add_argument('--arm-a', required=True)
    ap.add_argument('--arm-b', required=True)
    ap.add_argument('--frames', default='last')
    ap.add_argument('--out', default='')
    ap.add_argument('--threshold', type=int, default=None, help='per-pixel max channel difference (of 255) that counts (default 1 when both arms are fview, else 12)')
    ap.add_argument('--min-px', type=int, default=None, help='smallest region reported, pixels (default 4 when both arms are fview, else 40)')
    args = ap.parse_args()
    stamp = time.strftime('%Y%m%d_%H%M%S')
    src = Path(args.input)
    out_dir = Path(args.out) if args.out else MAIN / 'out' / 's101' / 'judge' / f'{src.stem if src.suffix else src.name}_{stamp}'
    unpack = MAIN / 'out' / 's101' / 'judge' / '_unpacked'
    unpack.mkdir(parents=True, exist_ok=True)
    cap = resolve_capture(src, unpack)
    arm_a, arm_b = Arm(args.arm_a, 'A'), Arm(args.arm_b, 'B')
    frames = parse_frames(args.frames, cap)
    judge(cap, arm_a, arm_b, frames, out_dir, args.threshold, args.min_px)


if __name__ == '__main__':
    main()
