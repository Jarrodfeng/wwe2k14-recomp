"""Selftest for q shader (S111 QGAPS). 0 game runs, no game bytes: PLANTED listings in the disassembler's own text shape and a planted fx.json.
Every lane rule has a planted twin (same operand, another destination mask / opcode) that must read different lanes. Exit 1 = a check failed.
WWE_QSHADER_PLANT=dest_mask_off switches the destination-mask rule off, so its twins are seen to fail."""
import contextlib
import io
import json
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qshader  # noqa: E402

fails = []


def check(what, ok, detail=''):
    print('%s %s%s' % ('ok  ' if ok else 'FAIL', what, '' if ok else '  ' + str(detail)))
    if not ok:
        fails.append(what)


def lanes(listing, reg):
    d = qshader.parse_reads(listing)['consts'].get(str(reg), {})
    return ''.join(l for l in 'xyzw' if l in d)


LISTING = """/*    0.0 */       exec
/*   10   */          add r3.xy__, c250.zwww, -c250.xyyy
              +       retain_prev r7._
/*   11   */          dp3 r5._y__, -r1.zxyy, c77.zxyy
/*   12   */          dp4 r4.x___, r3.wzxy, c58.zxyw
/*   13   */          dp2add r0.x___, r7.zwww, r1.xzzz, c255.wxxx
/*   14   */          mul r3.xy__, r8.xxxx, c169.xyzw
              +       mulsc r0.___w, c254.y, r3.x
/*   15   */     (p0) mul r9._yzw, r10.yyyy, c17.xxzy
              +       maxs r0._, c184.xx
/*   16   */          dp3 r9.x___, r6.zxyy, c[77+a0].zxyy
/*   17   */          tfetch2D r10.__xy, r5.xy, tf15, FetchValidOnly=false
/*   18   */          tfetchCube r10.wzxy, r16.xyz, tf13
/*   19   */          vfetch_full r3, r0.x, vf2
/*    5.0 */       cexec b1
"""


def main():
    r = qshader.parse_reads(LISTING)
    check('add: dest xy__ reads swizzle positions 0,1 of each source (z,w) + (x,y)', lanes(LISTING, 250) == 'xyzw', lanes(LISTING, 250))
    check('twin: the same operand under dest __zw reads positions 2,3 only',
          lanes('add r3.__zw, r1.xxxy, -c250.zwxy\n', 250) == 'xy', lanes('add r3.__zw, r1.xxxy, -c250.zwxy\n', 250))
    check('dp3 reads the first three swizzle chars whatever the dest mask (z,x,y, not w)', lanes(LISTING, 77) == 'xyz', lanes(LISTING, 77))
    check('twin: dp4 on zxyw reads all four', lanes(LISTING, 58) == 'xyzw', lanes(LISTING, 58))
    check('twin: dp3 on the same zxyw reads only x y z', lanes('dp3 r4.x___, r3.wzxy, c58.zxyw\n', 58) == 'xyz', lanes('dp3 r4.x___, r3.wzxy, c58.zxyw\n', 58))
    check('dp2add: the third source reads its first lane only (w of wxxx)', lanes(LISTING, 255) == 'w', lanes(LISTING, 255))
    check('twin: the same third source in dp2add as plain mul under a full dest reads w and x',
          lanes('mul r0, r1, c255.wxxx\n', 255) == 'xw', lanes('mul r0, r1, c255.wxxx\n', 255))
    check('mul under x y dest reads positions 0,1 of xyzw only', lanes(LISTING, 169) == 'xy', lanes(LISTING, 169))
    check('scalar-slot mulsc reads its printed lane (y)', lanes(LISTING, 254) == 'y', lanes(LISTING, 254))
    check('a predicated mul under _yzw reads positions 1..3 (x z y)', lanes(LISTING, 17) == 'xyz', lanes(LISTING, 17))
    check('a 2-lane scalar operand reads its lane set (xx -> x)', lanes(LISTING, 184) == 'x', lanes(LISTING, 184))
    check('indexed constants are apart from c77', list(r['indexed']) == ['77+a0'] and ''.join(sorted(r['indexed']['77+a0'])) == 'xyz', r['indexed'])
    check('a register operand is no constant', '1' not in r['consts'] and '7' not in r['consts'], list(r['consts']))
    check('samplers: 2D slot 15 and Cube slot 13 with their dest swizzle', [(s['slot'], s['kind']) for s in r['samplers']] == [(13, 'Cube'), (15, '2D')], r['samplers'])
    check('sampler dest swizzle and coordinate register kept', r['samplers'][1]['dest'] == ['r10.__xy'] and r['samplers'][1]['coord'] == ['r5.xy'], r['samplers'])
    check('vertex fetch slot and bool constant', r['vfetch'] == [2] and r['bools'] == [1], (r['vfetch'], r['bools']))
    # the whole verb: a planted fx.json, a planted disassembler, a planted cache
    tmp = tempfile.mkdtemp(prefix='qshader_')
    try:
        fx = os.path.join(tmp, 'src', 'renderer', 'shaders', 'fx')
        os.makedirs(fx)
        with open(os.path.join(fx, 'yTest.fx.json'), 'w') as f:
            json.dump({'effect': 'yTest', 'vertex': [], 'fragment': [{'name': 'muscle', 'hashes': ['0BFB1F5EA3F5102F']}, {'name': 'plain', 'hashes': ['0BFB1F5E11111111', 'AAAA000000000001']}]}, f)
        os.environ['WWE_QSHADER_CACHE'] = os.path.join(tmp, 'cache')
        calls = []

        def fake(effect, hashes, root=None):
            calls.append((effect, tuple(hashes)))
            return {h: ('PS', LISTING) for h in hashes}

        def run(argv):
            out, err = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                rc = qshader.main(argv + ['--root', tmp], disasm=fake)
            return rc, out.getvalue(), err.getvalue()

        rc, out, err = run(['yTest_muscle_fragment'])
        check('by variant name: one scan, the reads printed', rc == 0 and len(calls) == 1 and 'c250' in out and 'tf15' in out and 'tf13' in out, (rc, out[:300], err))
        rc, out, err = run(['0BFB1F5EA3F5102F'])
        check('by full hash: from the cache, no second scan', rc == 0 and len(calls) == 1 and 'c250' in out, (calls, err))
        cached = os.listdir(os.path.join(tmp, 'cache'))
        check('the cache holds the read list only (numbers, no listing)', cached == ['0BFB1F5EA3F5102F.json'] and 'exec' not in open(os.path.join(tmp, 'cache', cached[0])).read(), cached)
        rc, out, err = run(['0BFB1F5E'])
        check('a hash prefix shared by two shaders lists both (twin of the full hash)', rc == 0 and out.count('== ') == 2, out[:200])
        rc, out, err = run(['muscle', '--effect', 'yOther'])
        check('twin: the same variant under another effect is no hit', rc == 2 and 'no fx.json shader' in err, (rc, err))
        rc, out, err = run(['plain', '--json'])
        check('--json: 2 hashes of the variant, each with consts', rc == 0 and len(json.loads(out)['shaders']) == 2 and 'consts' in json.loads(out)['shaders'][0], out[:200])
    finally:
        os.environ.pop('WWE_QSHADER_CACHE', None)
        shutil.rmtree(tmp, ignore_errors=True)
    print('FAILED: %s' % fails if fails else 'all passed')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
