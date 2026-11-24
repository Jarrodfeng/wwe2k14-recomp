"""qgl_selftest: proves the S103 QGL work on fixtures in a TEMP folder (no game, 0 runs; the index tests went with the index, S110 QTEXT):
  glsl   `q ours glsl`: a uniform / an interpolator component / a define, with kind and the #if guard (#else inverted, a file's own include guard dropped);
         PLANTED twins: `inUvFog.xy` is NOT a hit for inUvFog.w, a comment naming it is not a hit, the #else twin of a write carries the INVERTED guard.
  fxreg  `q ours fxreg [--dupes]`: duplicate hashes grouped by owner (cross-owner flagged); a planted _fragment / _pl_fragment twin where only one side has a
         constants table is listed, the clean pair is not; a variant no hash row names is listed.
  str    `q ours str`: a literal, a printf-style literal found by its prefix (a different literal that starts the same IS reported, marked `prefix of a format`)
         and as two adjacent literals; PLANTED: a comment naming it is NOT a hit; a global cap stops a huge result.
  glsl2  (QGL review) write = nothing but an index / swizzle between the hit and the assignment (`f(inUvFog.w); z = 1` and `a == inUvFog.w` are reads), #elif =
         !(A) && B, whole-vector uses listed, backslash-continued #if, a directive inside a block comment ignored.
Run:  py -I tools/q/qgl_selftest.py
"""
import json
import os
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
Q = os.path.join(HERE, 'q.py')
ENV = dict(os.environ, WWE_Q_OURS_NO_AUTOUPDATE='1', PYTHONIOENCODING='utf-8')


class Fail(Exception):
    pass


def check(cond, what, out=''):
    if not cond:
        raise Fail(f'{what}\n--- output ---\n{str(out)[-1500:]}')
    print(f'PASS  {what}')


def write(root, rel, text):
    p = os.path.join(root, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)


def q(root, *a):
    r = subprocess.run([sys.executable, '-I', Q, 'ours'] + list(a) + ['--root', root], capture_output=True, text=True, env=ENV, timeout=120, errors='replace')
    return r.stdout + r.stderr


GLSL = '''#ifndef FX_SKIN_GLSL
#define FX_SKIN_GLSL
layout(location = 1) in vec4 inUvFog;
layout(set = 0, binding = 1) uniform FragConsts {
  vec4 Sweat;             // x amount
} fc;
#ifdef FX_WRINKLES
void wr() {
  normalMap += wrinkle * inUvFog.w;
}
#else
void wr() {
  normalMap = base * inUvFog.w;
}
#endif
void main() {
  vec2 uv = inUvFog.xy;
  // inUvFog.w is only named in this comment
  float s = fc.Sweat.y * 2.0;
  fc.Sweat.x = 1.0;
  inUvFog.w = 0.5;
  if (fog == 1.0) { }
}
#endif
'''


def registry():
    def v(name, consts):
        c = 'kC_' + name if consts else 'nullptr'
        return f'  {{"{name}", kFx_{name}, sizeof(kFx_{name}), nullptr, 0, {c}, {consts}, 16, 65535, nullptr, 0, nullptr, 0, 0, 255}}'
    names = [('yAudDefault_base_fragment', 1), ('yAudience_base_fragment', 1), ('yBG_Default_fragment', 1), ('yBG_Default_pl_fragment', 0),
             ('yProp_Default_fragment', 1), ('yProp_Default_pl_fragment', 1), ('yAlone_quad_vertex', 0), ('yNohash_vertex', 0)]
    vs = ',\n'.join(v(n, c) for n, c in names)
    hs = ['{0x00000000000000AAull, 0}', '{0x00000000000000AAull, 1}', '{0x00000000000000BBull, 2}', '{0x00000000000000CCull, 3}',
          '{0x00000000000000DDull, 4}', '{0x00000000000000EEull, 5}', '{0x00000000000000FFull, 6}']
    return ('#pragma once\nnamespace rdr::fx {\ninline constexpr Variant kVariants[] = {\n' + vs + '\n};\n'
            'inline constexpr HashEntry kHashes[] = {\n  ' + ',\n  '.join(hs) + '\n};\n}\n')


CPP = '''#include <cstdio>
void Bind(int i) {
    char name[64];
    std::snprintf(name, sizeof(name), "g_fWklLv%d", i);
    const char* a = "g_fWklLv" "%d";
    const char* c = "g_mNormalWklPrm";
    const char* d = "g_fWklLv0Extra";   // a different literal that merely starts the same
    // "g_mNormalWklPrm" is only named in this comment
}
'''


def test_text_queries(d):
    write(d, 'src/shaders/fx_skin.glsl', GLSL)
    out = q(d, 'glsl', 'inUvFog.w')
    check('4 lines' in out and 'glsl inUvFog.w' in out, 'glsl inUvFog.w: the 3 lines that touch w + the whole-vector declaration', out)
    check('[decl  ] layout(location = 1) in vec4 inUvFog;   (whole vector)' in out, 'a whole-vector line (the declaration) is listed and marked', out)
    check('inUvFog.xy' not in out, 'PLANTED twin: `inUvFog.xy` is not a hit for inUvFog.w', out)
    check('only named in this comment' not in out, 'PLANTED twin: a comment naming it is not a hit', out)
    check(out.count('[read  ]') == 2 and out.count('[write ]') == 1 and out.count('[decl  ]') == 1, 'two reads (right of an = / +=) and one write (inUvFog.w = ..)', out)
    check('#if defined(FX_WRINKLES)' in out and '#if !(defined(FX_WRINKLES))' in out, 'the #ifdef guard, and its #else twin carries the INVERTED guard', out)
    check('FX_SKIN_GLSL' not in out, "the file's own include guard is not shown as a feature guard", out)
    out = q(d, 'glsl', 'Sweat')
    check('[decl  ]' in out and out.count('[read  ]') == 1 and '[write ]' in out, 'glsl Sweat: the field decl, one read (.y), one write (.x)', out)
    out = q(d, 'glsl', 'FX_WRINKLES')
    check('[guard ]' in out and '1 guard' in out, 'glsl FX_WRINKLES: the #ifdef is a guard row', out)
    out = q(d, 'glsl', 'NoSuchName')
    check('0 lines' in out, 'glsl: an unknown name says so', out)

    write(d, 'src/renderer/shaders/spv/fx_registry.h', registry())
    out = q(d, 'fxreg', '--dupes')
    check('1 duplicate hashes (1 across owners)' in out, 'fxreg: the planted cross-owner duplicate hash is counted', out)
    check('owner yAudDefault,yAudience' in out and 'CROSS-OWNER' in out and '0x00000000000000AA' in out, 'fxreg --dupes: grouped by owner, flagged cross-owner', out)
    check('yBG_Default_pl_fragment [0]   -> no table: yBG_Default_pl_fragment' in out, 'fxreg: the planted twin with no constants table on one side is listed', out)
    check('yProp_Default' not in out.split('twins without a table')[-1].split('variants no hash')[0], 'fxreg: the clean twin pair (both with a table) is NOT listed', out)
    check('yNohash_vertex' in out, 'fxreg --dupes: a variant no hash row names is listed', out)
    out2 = q(d, 'fxreg')
    check('--dupes lists' in out2 and '0x00000000000000AA' not in out2, 'fxreg without --dupes only counts', out2)

    write(d, 'src/bind.cpp', CPP)
    out = q(d, 'str', 'g_fWklLv%d')
    check(out.count('<literal>') == 2 and 'a = "g_fWklLv" "%d"' in out and 'snprintf' in out, 'str "g_fWklLv%d": the printf literal and the split literals', out)
    check('g_fWklLv0Extra' in out and '<prefix of a format>' in out, 'str: a printf-style literal also matches by its prefix (reported as such)', out)
    out = q(d, 'str', 'g_mNormalWklPrm')
    check(out.count('src/bind.cpp:') == 1 and 'const char* c' in out, 'str "g_mNormalWklPrm": the real use', out)
    check('only named in this comment' not in out, 'PLANTED twin: a comment naming the literal is not a use', out)
    out = q(d, 'str', 'g_mNothingHere')
    check('0 string-literal sites' in out, 'str: an unused literal says so', out)


GLSL2 = '''#ifdef A_ON
void a() { }
#elif defined(B_ON) && \\
      defined(C_ON)
void b() { v = inUvFog.w; }
#else
void c() { inUvFog.w += 1.0; y = 2.0; }
#endif
/*
#ifdef HIDDEN_GUARD
*/
void d() {
  foo(inUvFog.w); z = 1.0;
  if (v == inUvFog.w) { k = 1.0; }
  inUvFog = vec4(0.0);
  out_c = texture(t, inUvFog.xy);
  g(inUvFog);
}
'''


def test_glsl2(d):
    write(d, 'src/shaders/g2.glsl', GLSL2)
    out = q(d, 'glsl', 'inUvFog.w')
    check('[write ] void c() { inUvFog.w += 1.0; y = 2.0; }' in out, 'review 11: `inUvFog.w += 1.0` is a write', out)
    check('[read  ] foo(inUvFog.w); z = 1.0;' in out, 'review 11: `foo(inUvFog.w); z = 1.0;` is a READ (the assignment after the ; is not the hit\'s)', out)
    check('[read  ] if (v == inUvFog.w)' in out, 'review 11: `v == inUvFog.w` is a read (== is not an assignment)', out)
    check('inUvFog = vec4(0.0)' in out and '(whole vector)' in out and 'g(inUvFog)' in out, 'review 12: whole-vector uses are listed for a component query', out)
    check('texture(t, inUvFog.xy)' not in out, 'review 12: a use of other components (.xy) is still skipped', out)
    check('[write ] inUvFog = vec4(0.0);   (whole vector)' in out, 'a whole-vector assignment is a write', out)
    check('#if !(defined(A_ON)) && defined(B_ON) && defined(C_ON)' in out, 'review 12: #elif (backslash-continued) = !(A) && B && C', out)
    check('#if !(defined(A_ON) || defined(B_ON) && defined(C_ON))' in out or '#if !(defined(A_ON) || defined(B_ON) && defined(C_ON))' in out.replace('  ', ' '),
          '#else = the negation of every earlier test of the chain', out)
    check('HIDDEN_GUARD' not in out and out.count('#if defined(HIDDEN_GUARD)') == 0, 'a #ifdef inside a block comment is not a guard', out)
    # the lines after the block comment carry NO guard of the commented-out ifdef
    check('#if defined(HIDDEN_GUARD)' not in out, 'no line sits under the commented-out #ifdef', out)


def main():
    d = tempfile.mkdtemp(prefix='qgl_selftest_')
    try:
        test_text_queries(os.path.join(d, 'fx'))
        test_glsl2(os.path.join(d, 'fx'))
    except Fail as e:
        print('FAIL ' + str(e))
        return 1
    finally:
        shutil.rmtree(d, ignore_errors=True)
    print('qgl_selftest OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
