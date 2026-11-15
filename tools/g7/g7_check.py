#!/usr/bin/env python3
"""S102 lane G7 proof (offline, 0 game runs, headless GPU compute through tools\\native\\compute_test, nothing on screen).
  py -I tools\\g7\\g7_check.py [--root DIR]
1. the skin-once kernel (src\\renderer\\shaders\\rdr_skin_once.comp) on the GPU against a numpy float32 model of the vertex shaders' blend, on REAL Bryan shadow vertices (frame 100's
   skinned 32-byte shadow-map draws) and a random palette, for 4 / 3 / 2 / 1 bones; 2. the planted twins: a wrong palette (one pass's palette for another's) and a wrong bone count must FAIL the same
   comparison; 3. the census: skinning once removes vertices (today > once > 0) on Bryan and the match, and the real input contract holds (position w = 1, weights sum <= 1, bone bytes < 60).
Exit 0 all pass. The numbers are in docs\\renderer\\SKIN_ONCE.md."""
import argparse, json, os, subprocess, sys
from pathlib import Path
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
fails = 0
MAIN_CO = ''


def line(ok, text):
    global fails
    print(('PASS  ' if ok else 'FAIL  ') + text, flush=True)
    fails += 0 if ok else 1


def model(verts, pal, bones, stride_dw=8, ids_dw=7):
    """float32 model of the blend: world = sum_k w_k * (rows of bone k . p); the last bone used takes 1 - the weights before it (vs_skin_shadow.vert)."""
    dw = verts.reshape(-1, stride_dw)
    f = dw.view('<f4')
    p = f[:, 0:4].astype(np.float32)
    ids = dw[:, ids_dw]
    pal = pal.reshape(256, 4)

    def bone(byte):
        a = np.clip(np.floor(byte.astype(np.float32) * np.float32(3.0) + np.float32(0.5)).astype(np.int64), -76, 177)
        return np.stack([(pal[76 + a] * p).sum(1, dtype=np.float32), (pal[77 + a] * p).sum(1, dtype=np.float32), (pal[78 + a] * p).sum(1, dtype=np.float32)], 1)

    b = [bone((ids >> (8 * k)) & 255) for k in range(4)]
    w = [f[:, 4], f[:, 5], f[:, 6]] if bones > 1 else [None, None, None]
    if bones >= 4:
        r = b[0] * w[0][:, None] + b[1] * w[1][:, None] + b[2] * w[2][:, None] + b[3] * (np.float32(1) - (w[0] + w[1] + w[2]))[:, None]
    elif bones == 3:
        r = b[0] * w[0][:, None] + b[1] * w[1][:, None] + b[2] * (np.float32(1) - (w[0] + w[1]))[:, None]
    elif bones == 2:
        r = b[0] * w[0][:, None] + b[1] * (np.float32(1) - w[0])[:, None]
    else:
        r = b[0]
    return np.concatenate([r, np.ones((len(r), 1), np.float32)], 1).astype('<f4')


def real_vertices(main_co, want=16000):
    sys.path.insert(0, os.path.join(ROOT, 'tools', 'shadow'))
    import shadow_census as sc
    z = os.path.join(main_co, 'out/s101/runs/20261112_122700_s100_owner_bryan_entrance/case_20261112_122719_969_0_pc_run_exit.zip')
    if not os.path.exists(z):
        raise SystemExit('FAIL input missing: ' + z)
    cap = sc.Cap(z)
    f = cap.frame(100); d = f.draws; o = f.origins
    ps = o['pass_site'].astype(np.int64)
    out, n = [], 0
    for i in np.nonzero(ps == sc.SITE_MAP)[0]:
        st, els = sc.layout_of(f, d[i])
        if st != 32 or [e[0] for e in els] != [109, 106, 41]:
            continue
        rows = np.ascontiguousarray(sc.vertex_rows(cap, d[i], st))
        out.append(rows.view('<u4').reshape(len(rows), 8))
        n += len(rows)
        if n >= want:
            break
    v = np.concatenate(out)[:want].copy()
    fl = v.view('<f4')
    line(len(v) > 1000 and bool((fl[:, 3] == 1.0).all()) and bool((fl[:, 4:7].sum(1) <= 1.0001).all()) and bool(((v[:, 7] & 255) < 60).all()),
         'real Bryan shadow vertices (frame 100): %d, position w == 1, weights sum <= 1, bone byte 0 < 60' % len(v))
    return v


def run_kernel(work, name, verts, pal_for_gpu, expect, bones):
    sys.path.insert(0, os.path.join(ROOT, 'tools', 'native', 'compute_test'))
    import compute_test as ct
    exe = Path(MAIN_CO) / 'out' / 'native' / 'compute_test' / 'run_compute.exe'   # built by the main checkout's compute_test build (a worktree has no out native folder)
    if not exe.exists():
        raise SystemExit('FAIL run_compute.exe missing: ' + str(exe))
    ct.EXE, ct.WORK, ct.ensure_exe = exe, work / 'cw', (lambda: None)
    n = len(verts)
    (work / (name + '_v.bin')).write_bytes(verts.tobytes())
    (work / (name + '_p.bin')).write_bytes(pal_for_gpu.astype('<f4').tobytes())
    (work / (name + '_e.bin')).write_bytes(expect.astype('<f4').tobytes())
    (work / (name + '_push.bin')).write_bytes(np.array([n, bones, 8, 7], '<u4').tobytes())
    spec = {'shader': os.path.relpath(os.path.join(ROOT, 'src', 'renderer', 'shaders', 'rdr_skin_once.comp'), work), 'groups': [(n + 63) // 64, 1, 1], 'push': name + '_push.bin',
            'bindings': [{'binding': 0, 'kind': 'ssbo', 'format': 'r32u', 'w': n * 32, 'in': name + '_v.bin'},
                         {'binding': 1, 'kind': 'ubo', 'format': 'r32f', 'w': 4096, 'in': name + '_p.bin'},
                         {'binding': 2, 'kind': 'ssbo', 'format': 'r32f', 'w': n * 16, 'in': None, 'expect': name + '_e.bin', 'tol': 5e-4}]}
    (work / (name + '.json')).write_text(json.dumps(spec))
    return ct.run(work / (name + '.json'))


def main():
    global fails
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    a = ap.parse_args()
    main_co = os.path.dirname(subprocess.run(['git', '-C', ROOT, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True).stdout.strip())
    global MAIN_CO
    MAIN_CO = main_co
    work = Path(ROOT) / 'out' / 'g7'
    work.mkdir(parents=True, exist_ok=True)
    verts = real_vertices(main_co)
    rng = np.random.default_rng(102)
    pal = rng.uniform(-1.5, 1.5, (256, 4)).astype('<f4')
    pal_other = pal.copy()
    pal_other[76 + 3 * 5: 76 + 3 * 6] += 0.05   # another pass's palette: bone 5 moved a little (the collapsed-bone case)
    for bones in (4, 3, 2, 1):
        exp = model(verts, pal, bones)
        rc = run_kernel(work, 'b%d' % bones, verts, pal, exp, bones)
        line(rc == 0, 'kernel on the GPU == model, %d bone(s), %d real vertices (tol 5e-4)' % (bones, len(verts)))
    # planted twins: the comparison must see them
    exp4 = model(verts, pal, 4)
    rc = run_kernel(work, 'twin_palette', verts, pal_other, exp4, 4)
    line(rc != 0, 'planted twin: the kernel fed another pass\'s palette (one bone differs) is CAUGHT by the same comparison')
    rc = run_kernel(work, 'twin_bones', verts, pal, exp4, 3)
    line(rc != 0, 'planted twin: a wrong bone count (3 for 4) is CAUGHT')
    # the census: skinning once removes vertices, on the capture and the match
    for cap in ('bryan', 'match'):
        r = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'g7_census.py'), '--capture', cap], capture_output=True, text=True)
        txt = r.stdout
        import re
        m = re.search(r'today (\d+) \(.*?\), skinned once (\d+) \(', txt)
        ok = r.returncode == 0 and m is not None and int(m.group(1)) > int(m.group(2)) > 0
        line(ok, 'census %s: shadow-layout vertices skinned a frame today %s, once %s' % (cap, m.group(1) if m else '?', m.group(2) if m else '?'))
    print('G7 CHECK: %s' % ('all passed' if not fails else '%d FAILED' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
