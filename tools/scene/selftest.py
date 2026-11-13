#!/usr/bin/env python3
"""Lane 6 acceptance test (offline, 0 game runs): the Arena Viewer's "rdr" backend against its own "vk" backend.

  py -I tools\\scene\\selftest.py [--root DIR] [--arena 5] [--size 640x360] [--keep]

Runs the viewer headless (--hidden) on one arena with the same camera on both backends, writes both shots and a side-by-side sheet to
out\\s101\\scene\\ (of --root), and compares them per tile (loose: "looks right", not byte-equal). Then the planted-fault twin: the rdr
backend with textures off (WWE_RDR_NO_TEXTURES=1) must FAIL the same comparison. Also checks the rdr run reported 0 validation errors
(WWE_RDR_VALIDATION=1) and that every effect line of the coverage list is either "bound to our effect" or "no effect yet".
Exit 0 only when: rdr passes, the faulty rdr fails, validation errors are 0, the coverage list is present.
S101 lane 13 (wrestlers): then three wrestlers in arena 5 in their fixed idle stance (one with cloth, one with gear, one with transparency),
vk vs rdr, compared over the wrestler's own region with the same tile bar; validation 0; the coverage of every yCh_* class counted (bound to our
effect, or listed "no effect yet" and drawn magenta); the planted fault (the palette off = the bind pose) must FAIL the same check for each; and
a labelled sheet out\\s101\\scene\\wrestlers_sheet.png (vk | rdr per wrestler).
A check that cannot read its input fails loudly (no shots / no viewer = FAIL, never a pass)."""
import argparse
import json
import os
import re
import struct
import subprocess
import sys
import zlib

MAIN = os.environ.get('RDR_MAIN_CHECKOUT') or os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))


def read_png(path):
    """Minimal PNG reader (8-bit RGB / RGBA, no interlace). Returns (w, h, rows of bytes as list of bytearray, channels)."""
    with open(path, 'rb') as f:
        data = f.read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise RuntimeError(f'{path}: not a PNG')
    pos, idat, w = 8, b'', None
    while pos < len(data):
        n, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if kind == b'IHDR':
            w, h, depth, ctype, _, _, interlace = struct.unpack('>IIBBBBB', body)
            if depth != 8 or ctype not in (2, 6) or interlace:
                raise RuntimeError(f'{path}: unsupported PNG (depth {depth}, type {ctype}, interlace {interlace})')
        elif kind == b'IDAT':
            idat += body
    ch = 4 if ctype == 6 else 3
    raw = zlib.decompress(idat)
    stride = w * ch
    rows, prev = [], bytearray(stride)
    p = 0
    for _ in range(h):
        ft = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        for i in range(stride):
            a = line[i - ch] if i >= ch else 0
            b = prev[i]
            c = prev[i - ch] if i >= ch else 0
            if ft == 1: line[i] = (line[i] + a) & 255
            elif ft == 2: line[i] = (line[i] + b) & 255
            elif ft == 3: line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif ft == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        rows.append(line)
        prev = line
    return w, h, rows, ch


def write_png(path, w, h, rows, ch=3):
    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xFFFFFFFF)
    raw = b''.join(b'\x00' + bytes(r) for r in rows)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2 if ch == 3 else 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


TILES_X, TILES_Y = 8, 6


def tile_stats(img):
    """Per tile: mean r, g, b, and the luminance standard deviation."""
    w, h, rows, ch = img
    out = []
    for ty in range(TILES_Y):
        for tx in range(TILES_X):
            x0, x1, y0, y1 = tx * w // TILES_X, (tx + 1) * w // TILES_X, ty * h // TILES_Y, (ty + 1) * h // TILES_Y
            n = (x1 - x0) * (y1 - y0)
            s = [0, 0, 0]
            ls = 0.0
            lq = 0.0
            for y in range(y0, y1):
                r = rows[y]
                for x in range(x0, x1):
                    o = x * ch
                    rr, gg, bb = r[o], r[o + 1], r[o + 2]
                    s[0] += rr; s[1] += gg; s[2] += bb
                    l = 0.3 * rr + 0.59 * gg + 0.11 * bb
                    ls += l; lq += l * l
            mean_l = ls / n
            out.append((s[0] / n, s[1] / n, s[2] / n, max(0.0, lq / n - mean_l * mean_l) ** 0.5))
    return out


def correlation(a, b):
    """Pearson correlation of the luminance of two images on a coarse grid (the picture's structure: where light and dark are)."""
    def grid(img, gx=64, gy=36):
        w, h, rows, ch = img
        v = []
        for j in range(gy):
            for i in range(gx):
                y = min(h - 1, (2 * j + 1) * h // (2 * gy)); x = min(w - 1, (2 * i + 1) * w // (2 * gx)) * ch
                r = rows[y]
                v.append(0.3 * r[x] + 0.59 * r[x + 1] + 0.11 * r[x + 2])
        return v
    va, vb = grid(a), grid(b)
    ma, mb = sum(va) / len(va), sum(vb) / len(vb)
    cov = sum((x - ma) * (y - mb) for x, y in zip(va, vb))
    da = sum((x - ma) ** 2 for x in va) ** 0.5
    db = sum((y - mb) ** 2 for y in vb) ** 0.5
    return cov / (da * db) if da > 1e-6 and db > 1e-6 else 0.0


def compare(ref, got):
    """Returns (ok, details). Loose bar: most tiles close in mean colour; none missing (flat/clear colour) or black where the reference has
    content; the picture's light/dark structure correlates."""
    if (ref[0], ref[1]) != (got[0], got[1]):
        return False, ['size differs']
    rs, gs = tile_stats(ref), tile_stats(got)
    bad, notes = 0, []
    close = 0
    for i, (r, g) in enumerate(zip(rs, gs)):
        d = (abs(r[0] - g[0]) + abs(r[1] - g[1]) + abs(r[2] - g[2])) / 3.0
        lr = 0.3 * r[0] + 0.59 * r[1] + 0.11 * r[2]
        lg = 0.3 * g[0] + 0.59 * g[1] + 0.11 * g[2]
        if d <= 40.0:
            close += 1
        if lg < 3.0 and lr > 25.0:
            bad += 1; notes.append(f'tile {i}: black (ref {lr:.0f})')
        elif g[3] < 2.0 and r[3] > 18.0:
            bad += 1; notes.append(f'tile {i}: flat (stddev {g[3]:.1f}, ref {r[3]:.1f})')
    frac = close / len(rs)
    corr = correlation(ref, got)
    notes.append(f'tiles close {close}/{len(rs)} ({frac:.2f}), missing/black/flat {bad}, structure correlation {corr:.3f}')
    ok = frac >= 0.80 and bad == 0 and corr >= 0.60
    return ok, notes


def run_viewer(exe, backend, game, outdir, arena, size, name, env_extra, timeout=300):
    w, h = size
    shot = os.path.join(outdir, f'{name}.png')
    if os.path.exists(shot):
        os.remove(shot)
    script = os.path.join(outdir, f'{name}.jsonl')
    with open(script, 'w') as f:
        for cmd in (
            {'cmd': 'look', 'mode': 'lit', 'shadows': False, 'dof': False, 'fxaa': False, 'bloom': False, 'glare': False, 'gamma': False, 'haze': False},
            {'cmd': 'load', 'kind': 'arena', 'id': arena},
            {'cmd': 'camera', 'preset': 'ring'},
            {'cmd': 'shot', 'path': shot, 'w': w, 'h': h},
            {'cmd': 'quit'},
        ):
            f.write(json.dumps(cmd) + '\n')
    env = dict(os.environ, **env_extra)
    log = os.path.join(outdir, f'{name}.err.txt')
    with open(os.path.join(outdir, f'{name}.out.txt'), 'w') as out, open(log, 'w') as err:
        p = subprocess.run([exe, '--backend', backend, '--game', game, '--script', script, '--exit', '--hidden'], stdout=out, stderr=err, env=env, timeout=timeout)
    return shot, log, p.returncode


# ---- S101 lane 13: wrestlers ---------------------------------------------------------------------------------------------------
# (wrestler id, attire, what the census (arena viewer, rdr coverage) found in it, the yCh_* classes the check expects the coverage list to hold)
WRESTLERS = [
    (197, 1, 'cloth', ('yCh_cloth', 'yCh_cloth_trans', 'yCh_skin')),       # Macho Man, attire 1: yCh_cloth + yCh_cloth_trans + yCh_trans
    (210, 1, 'gear', ('yCh_gear', 'yCh_skin_flex', 'yCh_Aniso_trans')),    # The Undertaker (American Badass), attire 1: yCh_gear x10
    (156, 0, 'transparency', ('yCh_trans', 'yCh_skin', 'yCh_gear')),       # Eddie Guerrero, attire 0: yCh_trans x12
]
CROP = (0.22, 0.78)  # the wrestler's region of the picture (columns, fraction of the width): the camera frames him in the middle


def run_scene(exe, backend, game, outdir, name, env_extra, cmds, timeout=300):
    shot = os.path.join(outdir, f'{name}.png')
    if os.path.exists(shot):
        os.remove(shot)
    script = os.path.join(outdir, f'{name}.jsonl')
    with open(script, 'w') as f:
        for cmd in cmds:
            c = dict(cmd)
            if c.get('cmd') == 'shot':
                c['path'] = shot.replace('\\', '/')
            f.write(json.dumps(c) + '\n')
    env = dict(os.environ, **env_extra)
    log = os.path.join(outdir, f'{name}.err.txt')
    with open(os.path.join(outdir, f'{name}.out.txt'), 'w') as out, open(log, 'w') as err:
        p = subprocess.run([exe, '--backend', backend, '--game', game, '--script', script, '--exit', '--hidden'], stdout=out, stderr=err, env=env, timeout=timeout)
    return shot, log, p.returncode


def wrestler_cmds(wid, attire, size):
    return [
        {'cmd': 'look', 'mode': 'lit', 'shadows': False, 'dof': False, 'fxaa': False, 'bloom': False, 'glare': False, 'gamma': False, 'haze': False},
        {'cmd': 'load', 'kind': 'arena', 'id': 5},
        {'cmd': 'place', 'wrestler': wid, 'attire': attire, 'spot': 'ring', 'id': 'w1', 'motion': 'idle', 'frame': 30},  # a fixed pose: the idle at frame 30
        {'cmd': 'spot_cam', 'actor': 'w1', 'dist': 300, 'side': 25, 'pitch': 8, 'fov': 40},
        {'cmd': 'shot', 'w': size[0], 'h': size[1]},
        {'cmd': 'quit'},
    ]


def compare_crop(np, ref, got, whole=False):  # S101 lane 15: whole = the whole picture, not the middle columns
    """The tile bar of compare(), over the wrestler's region only (columns CROP of the width), plus the share of pixels that differ a lot.
    ref, got: uint8 arrays (h, w, 3). Returns (ok, notes)."""
    if ref.shape != got.shape:
        return False, ['size differs']
    h, w, _ = ref.shape
    x0, x1 = (0, w) if whole else (int(w * CROP[0]), int(w * CROP[1]))
    r, g = ref[:, x0:x1].astype(np.float64), got[:, x0:x1].astype(np.float64)
    ch, cw = r.shape[:2]
    lum = np.array([0.3, 0.59, 0.11])
    close, bad, notes = 0, 0, []
    for ty in range(TILES_Y):
        for tx in range(TILES_X):
            ys, ye, xs, xe = ty * ch // TILES_Y, (ty + 1) * ch // TILES_Y, tx * cw // TILES_X, (tx + 1) * cw // TILES_X
            tr, tg = r[ys:ye, xs:xe], g[ys:ye, xs:xe]
            mr, mg = tr.reshape(-1, 3).mean(0), tg.reshape(-1, 3).mean(0)
            lr, lg = float(mr @ lum), float(mg @ lum)
            sr, sg = float((tr @ lum).std()), float((tg @ lum).std())
            if float(np.abs(mr - mg).mean()) <= 40.0:
                close += 1
            if lg < 3.0 and lr > 25.0:
                bad += 1
                notes.append(f'tile ({tx},{ty}): black (ref {lr:.0f})')
            elif sg < 2.0 and sr > 18.0:
                bad += 1
                notes.append(f'tile ({tx},{ty}): flat (stddev {sg:.1f}, ref {sr:.1f})')
    n = TILES_X * TILES_Y
    lr_, lg_ = r @ lum, g @ lum

    def grid(a, gx=48, gy=36):
        hh, ww = a.shape
        return np.array([[a[min(hh - 1, (2 * j + 1) * hh // (2 * gy)), min(ww - 1, (2 * i + 1) * ww // (2 * gx))] for i in range(gx)] for j in range(gy)]).ravel()
    va, vb = grid(lr_), grid(lg_)
    corr = float(np.corrcoef(va, vb)[0, 1]) if va.std() > 1e-6 and vb.std() > 1e-6 else 0.0
    differ = float((np.abs(lr_ - lg_) > 60.0).mean())
    notes.append(f'tiles close {close}/{n} ({close / n:.2f}), missing/black/flat {bad}, structure correlation {corr:.3f}, pixels off by > 60: {differ * 100:.1f}%')
    return close / n >= 0.80 and bad == 0 and corr >= 0.80 and differ <= 0.04, notes


def parse_coverage(text):
    """The rdr coverage list: {class: [state, draws, also_unbound]} for the yCh_* classes, and the 'effects: ...; draws: A bound, B debug colour' line."""
    cls = {}
    for m in re.finditer(r'^  (yCh_\S+)\s+(bound to our effect|no effect yet)\s+.*?\((\d+) draws', text, re.M):
        cls[m.group(1)] = [m.group(2), int(m.group(3)), 0]
    for m in re.finditer(r'^    also (\d+) draws of (yCh_\S+) in the debug colour', text, re.M):
        if m.group(2) in cls:
            cls[m.group(2)][2] += int(m.group(1))
    tot = re.findall(r'^effects: (\d+) of (\d+) bound to our effect; draws: (\d+) bound, (\d+) debug colour', text, re.M)
    return cls, (tot[-1] if tot else None)


def run_wrestlers(exe, game, outdir, size, fails):
    try:
        import numpy as np
        from PIL import Image, ImageDraw
    except ImportError as e:
        print(f'FAIL  wrestlers: needs numpy and Pillow ({e})')
        fails.append('wrestlers: no numpy / Pillow')
        return
    panels = []
    for wid, attire, kind, expect in WRESTLERS:
        tag = f'w{wid}_{attire}'
        cmds = wrestler_cmds(wid, attire, size)
        got = {}
        for name, backend, env in ((f'{tag}_vk', 'vk', {}), (f'{tag}_rdr', 'rdr', {'WWE_RDR_VALIDATION': '1'}), (f'{tag}_nopal', 'rdr', {'WWE_RDR_VALIDATION': '1', 'WWE_RDR_NO_PALETTE': '1'})):
            try:
                shot, log, code = run_scene(exe, backend, game, outdir, name, env, cmds)
            except subprocess.TimeoutExpired:
                print(f'FAIL  {name}: viewer timed out')
                fails.append(f'{name}: timed out')
                break
            if not os.path.exists(shot):
                print(f'FAIL  {name}: no shot written (exit {code}); see {log}')
                fails.append(f'{name}: no shot')
                break
            with open(log, errors='replace') as f:
                got[name] = (np.asarray(Image.open(shot).convert('RGB')), f.read())
        if len(got) < 3:
            continue
        ref, rdr, nopal = got[f'{tag}_vk'][0], got[f'{tag}_rdr'], got[f'{tag}_nopal'][0]
        ok, notes = compare_crop(np, ref, rdr[0])
        print(('PASS' if ok else 'FAIL') + f'  wrestler {wid}/{attire} ({kind}) rdr vs vk: ' + '; '.join(notes))
        if not ok:
            fails.append(f'wrestler {wid}/{attire}: rdr differs from vk')
        twin_ok, twin_notes = compare_crop(np, ref, nopal)
        print(('PASS' if not twin_ok else 'FAIL') + f'  wrestler {wid}/{attire} planted fault (palette off = bind pose) is caught: ' + '; '.join(twin_notes))
        if twin_ok:
            fails.append(f'wrestler {wid}/{attire}: the planted fault passed the check')
        errs = [int(m) for m in re.findall(r'validation errors(?: so far)?: (\d+)', rdr[1])]
        if not errs or max(errs) != 0:
            print(f'FAIL  wrestler {wid}/{attire}: rdr validation errors: {max(errs) if errs else "no validation line"}')
            fails.append(f'wrestler {wid}/{attire}: validation')
        else:
            print(f'PASS  wrestler {wid}/{attire}: rdr validation errors: 0')
        cls, tot = parse_coverage(rdr[1])
        if not cls or tot is None:
            print(f'FAIL  wrestler {wid}/{attire}: no yCh_* coverage list in the rdr log')
            fails.append(f'wrestler {wid}/{attire}: no coverage')
        else:
            bound = sum(v[1] for v in cls.values() if v[0] == 'bound to our effect')
            unbound = sum(v[1] for v in cls.values() if v[0] != 'bound to our effect') + sum(v[2] for v in cls.values())
            missing = [e for e in expect if e not in cls]
            line = ', '.join(f'{k} {v[1]}' + ('' if v[0] == 'bound to our effect' else ' (no effect yet)') + (f' +{v[2]} magenta' if v[2] else '') for k, v in sorted(cls.items()))
            if missing:
                print(f'FAIL  wrestler {wid}/{attire}: expected classes missing from the coverage list: {missing} (the census changed?)')
                fails.append(f'wrestler {wid}/{attire}: coverage classes')
            else:
                print(f'PASS  wrestler {wid}/{attire} coverage: {bound} character draws bound to our effect, {unbound} listed "no effect yet" (magenta): {line}')
        panels.append((f'{wid}/{attire} {kind}', ref, rdr[0]))
    if panels:
        label_h, gap = 16, 4
        h, w = panels[0][1].shape[:2]
        sheet = Image.new('RGB', (2 * w + gap, len(panels) * (h + label_h)), (32, 32, 32))
        d = ImageDraw.Draw(sheet)
        for i, (label, a, b) in enumerate(panels):
            y = i * (h + label_h)
            d.text((4, y + 2), f"vk  (the viewer's own picture)  wrestler {label}", fill=(255, 255, 255))
            d.text((w + gap + 4, y + 2), f"rdr (our renderer, the game's own effects)  wrestler {label}", fill=(255, 255, 255))
            sheet.paste(Image.fromarray(a), (0, y + label_h))
            sheet.paste(Image.fromarray(b), (w + gap, y + label_h))
        path = os.path.join(outdir, 'wrestlers_sheet.png')
        sheet.save(path)
        print(f'sheet: {path}  (left: vk, right: rdr; one row per wrestler)')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')))
    ap.add_argument('--arena', type=int, default=5)
    ap.add_argument('--size', default='640x360')
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    exe = os.path.join(root, 'out', 'tools', 'wwe2k14_assets_rdr.exe')
    game = os.path.join(MAIN, 'game_data', 'extracted')
    for need in (exe, game):
        if not os.path.exists(need):
            print(f'FAIL  cannot read its input: {need}')
            return 1
    outdir = os.path.join(root, 'out', 's101', 'scene')
    os.makedirs(outdir, exist_ok=True)
    size = tuple(int(x) for x in args.size.split('x'))
    fails = []

    shots = {}
    for name, backend, env in (('vk', 'vk', {}), ('rdr', 'rdr', {'WWE_RDR_VALIDATION': '1'}), ('rdr_notex', 'rdr', {'WWE_RDR_VALIDATION': '1', 'WWE_RDR_NO_TEXTURES': '1'})):
        try:
            shot, log, code = run_viewer(exe, backend, game, outdir, args.arena, size, name, env)
        except subprocess.TimeoutExpired:
            print(f'FAIL  {name}: viewer timed out')
            return 1
        if not os.path.exists(shot):
            print(f'FAIL  {name}: no shot written (exit {code}); see {log}')
            return 1
        shots[name] = (read_png(shot), log)

    ref = shots['vk'][0]
    ok, notes = compare(ref, shots['rdr'][0])
    print(('PASS' if ok else 'FAIL') + '  rdr vs vk: ' + '; '.join(notes))
    if not ok:
        fails.append('rdr differs from vk')
    twin_ok, twin_notes = compare(ref, shots['rdr_notex'][0])
    print(('PASS' if not twin_ok else 'FAIL') + '  planted fault (textures off) is caught: ' + '; '.join(twin_notes))
    if twin_ok:
        fails.append('the planted fault passed the check')

    # validation + coverage from the rdr run's stderr
    with open(shots['rdr'][1], errors='replace') as f:
        text = f.read()
    errs = [int(m) for m in re.findall(r'validation errors(?: so far)?: (\d+)', text)]
    if not errs:
        print('FAIL  no "validation errors" line in the rdr log')
        fails.append('no validation line')
    elif max(errs) != 0:
        print(f'FAIL  rdr validation errors: {max(errs)}')
        fails.append('validation errors')
    else:
        print('PASS  rdr validation errors: 0')
    cov = re.findall(r'^  (\S+)\s+(bound to our effect|no effect yet)', text, re.M)
    summary = re.findall(r'^effects: (\d+) of (\d+) bound to our effect', text, re.M)
    if not cov or not summary:
        print('FAIL  no coverage list in the rdr log')
        fails.append('no coverage list')
    else:
        print(f'PASS  coverage list: {summary[-1][0]} of {summary[-1][1]} effects bound to our effect')

    # the sheet: vk | rdr side by side
    a, b = shots['vk'][0], shots['rdr'][0]
    rows = []
    for y in range(a[1]):
        ra = bytearray(a[2][y][i] for i in range(a[0] * a[3]) if a[3] == 3 or i % 4 != 3)
        rb = bytearray(b[2][y][i] for i in range(b[0] * b[3]) if b[3] == 3 or i % 4 != 3)
        rows.append(ra + bytearray(b'\xff\xff\xff' * 4) + rb)
    sheet = os.path.join(outdir, 'arena_sheet.png')
    write_png(sheet, a[0] * 2 + 4, a[1], rows)
    print(f'sheet: {sheet}  (left: vk, right: rdr)')
    run_wrestlers(exe, game, outdir, size, fails)  # S101 lane 13
    if fails:
        print('FAIL  ' + '; '.join(fails))
        return 1
    print('PASS  lane 6 selftest')
    return 0


if __name__ == '__main__':
    sys.exit(main())
