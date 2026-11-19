#!/usr/bin/env python3
r"""Twin of compile_shaders.py's content-hash cache (S103 GSP). 0 game runs. Works on a temp copy of the shader tree, never the real one.

  py -I tools\renderer\spv_cache_twin.py

  1. cold --check: everything compiles (reused 0), headers current;
  2. warm --check: nothing compiles (compiled 0), headers current, same verdict;
  3. a comment added to one plain .frag: that one shader recompiles and the verdict stays current (the key changed, the words did not);
  4. a real code change in that .frag (a literal): it recompiles and --check FAILS naming that header (the cache cannot hide a stale committed header);
  5. a comment added to an include (fx_trans_ps.glsl): only the shaders that reach it recompile (some, not all);
  6. a damaged cache entry is a miss, never a result;
  7. (GSP2 review bug 4) a FOREIGN entry (another key's file copied under a current key) and a BARE valid-magic .spv dropped under a current key are rejected (counted, recompiled once), the committed-header verdict does not change;
  8. (bug 4a) an include edited between a shader's key and its compile (SPV_TWIN_EDIT, a planted hook) is NOT stored: after the files are put back the shader compiles again (its old key was never filled);
  9. (bug 4c) SPV_CACHE=0 --check on the same tree: headers current, nothing reused (the cache can never be what says "current");
  10. (bug 5) lint of the real tree: no file name in both src\renderer\shaders and fx, no shader subfolders besides fx and spv, plain quoted include names only (glslang searches the including file's own
      folder first; the key does not model that, so the tree must not need it).
Exit 0 = all hold."""
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
LAST = {'rejected': -1, 'unstored': -1}
HEADER = 6 + 8 + 32   # entry magic + length + sha256 (compile_shaders.pack)


def include_lint():
    """Bug 5, over the real tree. Returns a list of problems."""
    src = os.path.join(ROOT, 'src', 'renderer', 'shaders')
    fx = os.path.join(src, 'fx')
    bad = []
    subs = sorted(d for d in os.listdir(src) if os.path.isdir(os.path.join(src, d)))
    if subs != ['fx', 'spv']:
        bad.append('shader subfolders are %s (only fx and spv are modelled)' % subs)
    if [d for d in os.listdir(fx) if os.path.isdir(os.path.join(fx, d))]:
        bad.append('a subfolder inside fx')
    both = sorted({f for f in os.listdir(src) if os.path.isfile(os.path.join(src, f))} & {f for f in os.listdir(fx) if os.path.isfile(os.path.join(fx, f))})
    if both:
        bad.append('file names in both src\\renderer\\shaders and fx: %s' % both)
    for folder in (src, fx):
        for f in os.listdir(folder):
            if not f.endswith(('.glsl', '.frag', '.vert', '.comp')):
                continue
            text = open(os.path.join(folder, f), encoding='utf-8').read()
            for m in re.finditer(r'^[ \t]*#[ \t]*include[ \t]+(.*)$', text, re.M):
                arg = m.group(1).strip()
                q = re.fullmatch(r'"([A-Za-z0-9_.]+)"', arg)
                if not q:
                    bad.append('%s: include %s is not a plain quoted name' % (f, arg))
                elif not (os.path.exists(os.path.join(src, q.group(1))) or os.path.exists(os.path.join(fx, q.group(1)))):
                    bad.append('%s: include %s is in neither folder' % (f, q.group(1)))
    return bad


def check(tmp, env_extra=None):
    env = dict(os.environ, SPV_CACHE_DIR=os.path.join(tmp, 'cache'))
    env.update(env_extra or {})
    t = time.time()
    p = subprocess.run([sys.executable, '-I', os.path.join(tmp, 'tools', 'renderer', 'compile_shaders.py'), '--check'], capture_output=True, text=True, env=env, errors='replace')
    out = p.stdout + p.stderr
    m = re.search(r'(\d+) compiles reused, (\d+) compiled', out)
    stale = re.search(r'STALE headers: (.*)', out)
    r = re.search(r'(\d+) damaged entries rejected, (\d+) not stored', out)
    LAST['rejected'], LAST['unstored'] = (int(r.group(1)), int(r.group(2))) if r else (-1, -1)
    return p.returncode, int(m.group(2)) if m else -1, int(m.group(1)) if m else -1, stale.group(1) if stale else '', time.time() - t, out


def main():
    bad = []
    outdir = os.path.join(ROOT, 'out')
    os.makedirs(outdir, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix='spv_twin_', dir=outdir)
    try:
        os.makedirs(os.path.join(tmp, 'tools', 'renderer'))
        shutil.copy(os.path.join(ROOT, 'tools', 'renderer', 'compile_shaders.py'), os.path.join(tmp, 'tools', 'renderer'))
        shutil.copytree(os.path.join(ROOT, 'src', 'renderer', 'shaders'), os.path.join(tmp, 'src', 'renderer', 'shaders'))
        src = os.path.join(tmp, 'src', 'renderer', 'shaders')

        def expect(name, ok, why):
            print('%s %s: %s' % ('PASS' if ok else 'FAIL', name, why))
            if not ok:
                bad.append(name)

        rc, comp, reused, stale, secs, out = check(tmp)
        distinct, total = comp, comp + reused   # (identical variants share a key: a cold run already reuses some)
        expect('cold', rc == 0 and comp > 0, 'rc %d, compiled %d, reused %d, %.0fs, stale [%s]' % (rc, comp, reused, secs, stale))
        rc, comp, reused, stale, secs, out = check(tmp)
        expect('warm', rc == 0 and comp == 0 and reused == total, 'rc %d, compiled %d, reused %d, %.0fs' % (rc, comp, reused, secs))
        frag = os.path.join(src, 'rdr_2d_quad.frag')
        orig = open(frag, encoding='utf-8', newline='').read()
        open(frag, 'w', encoding='utf-8', newline='').write(orig + '\n// twin comment\n')
        rc, comp, reused, stale, secs, out = check(tmp)
        expect('comment_in_one_frag', rc == 0 and comp == 1, 'rc %d, compiled %d (want 1), reused %d, %.0fs' % (rc, comp, reused, secs))
        caught = False
        for lit in re.findall(r'\b\d+\.\d+\b', orig)[:12]:
            open(frag, 'w', encoding='utf-8', newline='').write(orig.replace(lit, '%.4f' % (float(lit) + 0.1234), 1))
            rc, comp, reused, stale, secs, out = check(tmp)
            if rc != 0 and 'rdr_2d_quad_frag.h' in stale and comp == 1:
                caught = True
                break
        expect('code_change_in_one_frag', caught, 'rc %d, compiled %d, stale [%s] (the committed header must be reported stale)' % (rc, comp, stale))
        open(frag, 'w', encoding='utf-8', newline='').write(orig)
        rc, comp, reused, stale, secs, out = check(tmp)
        expect('restored', rc == 0 and comp == 0, 'rc %d, compiled %d (the original text is still cached), %.0fs' % (rc, comp, secs))
        inc = os.path.join(src, 'fx', 'fx_trans_ps.glsl')
        io = open(inc, encoding='utf-8', newline='').read()
        open(inc, 'w', encoding='utf-8', newline='').write(io + '\n// twin comment\n')
        rc, comp, reused, stale, secs, out = check(tmp)
        expect('comment_in_include', rc == 0 and 0 < comp < distinct, 'rc %d, compiled %d of %d (only the includers), %.0fs' % (rc, comp, distinct, secs))
        open(inc, 'w', encoding='utf-8', newline='').write(io)
        check(tmp)
        # a damaged entry is a miss: the OLDEST .spv is a cold-run entry, still current (every edit above was restored), so exactly one compile happens
        files = sorted((os.path.getmtime(os.path.join(tmp, 'cache', f)), f) for f in os.listdir(os.path.join(tmp, 'cache')) if f.endswith('.spv'))
        open(os.path.join(tmp, 'cache', files[0][1]), 'wb').write(b'X' * 7)
        rc, comp, reused, stale, secs, out = check(tmp)
        expect('damaged_entry', rc == 0 and comp == 1, 'rc %d, compiled %d (want 1: the damaged entry is a miss, never a result)' % (rc, comp))
        # 7: a foreign entry and a bare valid-magic .spv under a current key. X = the oldest entry (current), Y = the largest entry of another file.
        cdir = os.path.join(tmp, 'cache')
        spvs = sorted((os.path.getmtime(os.path.join(cdir, f)), f) for f in os.listdir(cdir) if f.endswith('.spv'))
        x = spvs[0][1]
        y = max((f for _, f in spvs if f != x), key=lambda f: os.path.getsize(os.path.join(cdir, f)))
        yblob = open(os.path.join(cdir, y), 'rb').read()
        open(os.path.join(cdir, x), 'wb').write(yblob)   # another key's whole entry under X's key
        rc, comp, reused, stale, secs, out = check(tmp)
        expect('foreign_entry', rc == 0 and comp == 1 and LAST['rejected'] >= 1 and not stale, 'rc %d, compiled %d (want 1), rejected %d (want >= 1), stale [%s]' % (rc, comp, LAST['rejected'], stale))
        open(os.path.join(cdir, x), 'wb').write(yblob[HEADER:])   # bare SPIR-V words (valid magic), no header
        rc, comp, reused, stale, secs, out = check(tmp)
        expect('bare_spirv_entry', rc == 0 and comp == 1 and LAST['rejected'] >= 1 and not stale, 'rc %d, compiled %d (want 1), rejected %d (want >= 1), stale [%s]' % (rc, comp, LAST['rejected'], stale))
        # 8: an include edited between the key and the compile is not stored (rdr_light_cubes.comp includes fx_prelight.glsl: one shader, one compile, so the hook fires on it)
        comp_src = os.path.join(src, 'rdr_light_cubes.comp')
        pre = os.path.join(src, 'fx', 'fx_prelight.glsl')
        co, po = open(comp_src, encoding='utf-8', newline='').read(), open(pre, encoding='utf-8', newline='').read()
        open(comp_src, 'w', encoding='utf-8', newline='').write(co + '\n// raced twin A\n')
        rc, comp, reused, stale, secs, out = check(tmp, {'SPV_TWIN_EDIT': pre + '|\\n// raced twin B\\n'})
        expect('race_not_stored', rc == 0 and LAST['unstored'] >= 1, 'rc %d, compiled %d, not stored %d (want >= 1)' % (rc, comp, LAST['unstored']))
        open(pre, 'w', encoding='utf-8', newline='').write(po)   # the include back to the text the build started under
        rc, comp, reused, stale, secs, out = check(tmp)
        expect('race_key_empty', rc == 0 and comp == 1, 'rc %d, compiled %d (want 1: the old key was never filled by the raced build), stale [%s]' % (rc, comp, stale))
        open(comp_src, 'w', encoding='utf-8', newline='').write(co)
        # 9: the uncached check says the same
        rc, comp, reused, stale, secs, out = check(tmp, {'SPV_CACHE': '0'})
        expect('cache_off_check', rc == 0 and reused == 0 and not stale and 'headers are current' in out, 'rc %d, compiled %d, reused %d, %.0fs, stale [%s]' % (rc, comp, reused, secs, stale))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    lint = include_lint()
    print('%s include lint (bug 5): %s' % ('PASS' if not lint else 'FAIL', 'clean' if not lint else '; '.join(lint)))
    if lint:
        bad.append('include_lint')
    print('spv cache twin: %d failed' % len(bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
