r"""S102 lane V1 (docs\renderer\DEVICE_LOST.md section 8): the crowd card bake's out-of-range constant read, gone and proved offline. 0 game runs.
  py -I tools\renderer\bone_range_check.py [--root DIR] [--base REV]
Needs this checkout's rplay (out\renderer\dev\rplay.exe, built by the lane's build check) and the MAIN checkout's captures (Brock out\lane86\brock\capture, Bryan out\lane86\bryan\capture).
The tree is never modified: the "before" is an rplay built from the commit this lane started from (BASE, d91221d3 = s102-arc-pt2 before V1), exported with `git archive` into
out\renderer\v1_base\<rev> (with a copy of the two header folders build.ps1 needs: stb, xxHash) and built there with its committed shader headers (build.ps1 -NoShaders). It has EVERY
old shader line, so the comparison covers every clamp this lane added (ch_bone, yMovieDefault, yPlaneShadowMap, rdr_plane_mask, yCircleShadow), not only ch_bone. A killed check leaves
nothing behind in the tree (`git diff --exit-code src\renderer\shaders` stays clean).
  fix arm      rplay --validation 3 (GPU-assisted) on both captures: exit 0, `validation errors 0`, no "access out of bounds" message.
  base arm     the same run with the base rplay must bring the message back on both captures (exit 4, "8192 bytes were bound"): the planted twin.
  pictures     per-frame --frame-hashes of the fix and the base rplay (no validation): EQUAL on every frame of both captures (Brock 341, Bryan 237).
  census       bone_range_census.py: every vertex id past the palette that the program reads carries weight 0 (the out-of-range read is `0 * x`).
Exit 1 on any failure."""
import argparse, os, re, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
BASE = 'd91221d3'
MIP_FROM, MIP_TO = '3004d80ff', 'daf91b9fe'   # the diffuse LOD-bias change (S103 MIP)
FAILS = []


def say(ok, text):
    print(('PASS  ' if ok else 'FAIL  ') + text, flush=True)
    if not ok:
        FAILS.append(text)


def sh(cmd, timeout, cwd=None):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, encoding='utf-8', errors='replace', cwd=cwd)


def main_checkout(root):
    try:
        common = sh(['git', '-C', root, 'rev-parse', '--path-format=absolute', '--git-common-dir'], 30).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return root


def base_rplay(root, main_co, rev):
    full = sh(['git', '-C', root, 'rev-parse', rev], 30).stdout.strip()
    if not full:
        say(False, 'base revision %s not found' % rev)
        return None
    dest = os.path.join(root, 'out', 'renderer', 'v1_base', full[:10] + '_mip2')   # S103 MIP: the base carries the diffuse LOD change (below) but keeps the old bone lines
    exe = os.path.join(dest, 'out', 'renderer', 'dev', 'rplay.exe')
    if os.path.exists(exe):
        return exe
    os.makedirs(dest, exist_ok=True)
    tar = os.path.join(dest, 'base.tar')
    p = sh(['git', '-C', root, 'archive', '-o', tar, full, 'src', 'tools/renderer', 'tools/fview'], 300)
    if p.returncode != 0:
        say(False, 'git archive failed: ' + p.stderr[-200:])
        return None
    p = sh(['tar', '-xf', tar, '-C', dest], 300)
    os.remove(tar)
    if p.returncode != 0:
        say(False, 'tar failed: ' + p.stderr[-200:])
        return None
    for lib in ('stb', 'xxHash'):   # build.ps1 looks for these under <repo>/thirdparty/rexglue-sdk-src/thirdparty; a copy, not a junction
        src = os.path.join(main_co, 'thirdparty', 'rexglue-sdk-src', 'thirdparty', lib)
        dst = os.path.join(dest, 'thirdparty', 'rexglue-sdk-src', 'thirdparty', lib)
        if os.path.isdir(src) and not os.path.isdir(dst):
            shutil.copytree(src, dst)
    # S103 MIP (2026-11-18): the diffuse now samples at computed LOD + the register bias, as the game does (docs/research/s103/SKM.md). That moves the wrestlers' pictures in BOTH arms, so the base gets the same
    # change on top of its OLD bone lines: the glsl diff of MIP_FROM..MIP_TO (texture reads only) applied to the base's fx sources, then the base's own compile_shaders rebuilds its headers.
    # The planted twin (old bone lines: out-of-bounds, exit 4) is untouched by it; the check below still requires it to fail.
    pd = sh(['git', '-C', root, 'diff', MIP_FROM, MIP_TO, '--', 'src/renderer/shaders/fx/fx_skin.glsl', 'src/renderer/shaders/fx/fx_trans_ps.glsl', 'src/renderer/shaders/fx/fx_bg.glsl'], 60)
    if pd.returncode != 0 or not pd.stdout.strip():
        say(False, 'the MIP diff %s..%s could not be read: %s' % (MIP_FROM, MIP_TO, pd.stderr[-200:]))
        return None
    pf = os.path.join(dest, 'mip.diff')
    with open(pf, 'w', encoding='utf-8', newline='') as f:
        f.write(pd.stdout)
    pa = sh(['patch', '-p1', '-l', '-i', 'mip.diff'], 60, cwd=dest)   # not git apply: inside a repo it skips an ignored folder silently; not --binary: the archive's files are CRLF
    if pa.returncode != 0:
        say(False, 'the MIP diffuse change does not apply to the base shaders: ' + (pa.stdout + pa.stderr)[-300:])
        return None
    pc = sh([sys.executable, '-I', os.path.join(dest, 'tools', 'renderer', 'compile_shaders.py')], 2400, cwd=dest)
    if pc.returncode != 0:
        say(False, 'the base shaders did not compile: ' + (pc.stdout + pc.stderr)[-300:])
        return None
    p = sh(['pwsh', '-NoProfile', '-File', os.path.join(dest, 'tools', 'renderer', 'build.ps1'), '-NoShaders'], 2400, cwd=dest)
    if p.returncode != 0 or not os.path.exists(exe):
        say(False, 'the base rplay did not build: ' + (p.stdout + p.stderr)[-300:])
        return None
    return exe


def rplay(exe, cap, frames, tag, tmp, validation):
    csv = os.path.join(tmp, tag + '.csv')
    cmd = [exe, '--capture', cap, '--frames', str(frames), '--present-size', '1280x720', '--out', os.path.join(tmp, tag + '.png')]
    cmd += ['--validation', '3'] if validation else ['--frame-hashes', csv]
    p = sh(cmd, 1500)
    text = p.stdout + p.stderr
    with open(os.path.join(tmp, tag + '.txt'), 'w', encoding='utf-8') as f:
        f.write(text)
    m = re.search(r'validation errors (\d+)', text)
    return {'rc': p.returncode, 'oob': len(re.findall(r'access out of bounds', text)), 'errors': int(m.group(1)) if m else -1,
            'hashes': open(csv).read() if (not validation and os.path.exists(csv)) else None, 'bound': '8192 bytes were bound' in text}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.normpath(os.path.join(HERE, '..', '..')))
    ap.add_argument('--base', default=BASE)
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    main_co = main_checkout(root)
    caps = [('brock', os.path.join(main_co, 'out', 'lane86', 'brock', 'capture'), 341), ('bryan', os.path.join(main_co, 'out', 'lane86', 'bryan', 'capture'), 237)]
    for n, c, _ in caps:
        if not os.path.isdir(c):
            say(False, 'capture %s missing: %s' % (n, c))
            return 1
    exe = os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')
    if not os.path.exists(exe):
        say(False, 'no rplay at %s' % exe)
        return 1
    base = base_rplay(root, main_co, a.base)
    if not base:
        return 1
    tmp = tempfile.mkdtemp(prefix='v1_bone_')
    for n, c, fr in caps:
        r = rplay(exe, c, fr, 'fix_v_' + n, tmp, True)
        say(r['rc'] == 0 and r['errors'] == 0 and r['oob'] == 0, 'fix %s: exit %d, validation errors %d, out-of-bounds messages %d (GPU-assisted validation)' % (n, r['rc'], r['errors'], r['oob']))
        r = rplay(base, c, fr, 'base_v_' + n, tmp, True)
        say(r['rc'] == 4 and r['oob'] > 0 and r['bound'], 'base %s (the planted twin: every old shader line): exit %d, %d out-of-bounds messages naming the 8192-byte range' % (n, r['rc'], r['oob']))
        h1 = rplay(exe, c, fr, 'fix_h_' + n, tmp, False)
        h0 = rplay(base, c, fr, 'base_h_' + n, tmp, False)
        eq = bool(h1['hashes']) and h1['hashes'] == h0['hashes'] and h1['rc'] == 0 and h0['rc'] == 0
        say(eq, 'pictures %s: per-frame hashes of the fix equal the base on all %d frames (%d csv lines)' % (n, fr, len((h1['hashes'] or '').splitlines())))
    for n, c, _ in caps:
        p = sh([sys.executable, '-I', os.path.join(HERE, 'bone_range_census.py'), c], 600)
        m = re.search(r"largest \|weight\| at such a slot x/y/z/w \[(.*)\]", p.stdout)
        ws = [float(x.strip(" '")) for x in m.group(1).split(',')] if m else None
        say(p.returncode == 0 and ws is not None and all(w == 0.0 for w in ws), 'census %s: %s' % (n, p.stdout.strip().split(': ', 1)[-1] if p.stdout else p.stderr[-200:]))
    print('bone_range_check: %d failure(s)' % len(FAILS))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
