r"""S102 V1: every dynamic index into a declared-size array of a uniform block in our hand-written shaders (our translation of the 360's a0-relative constant reads),
and whether the index is PROVED to stay inside the array.

  py -I tools\renderer\const_index_lint.py [--root DIR] [--list] [--plant]
Scans src\renderer\shaders and src\scene (.glsl .vert .frag .comp). A site is `<instance>.<member>[expr]` (and the bare `CH_BONES[expr]`) where member is an array declared with a
size in the same file (`vec4 WorldMatrixArray[63];`; runtime-sized buffer arrays `[]` are bound by the buffer, not here) and expr is not an integer literal.
The index expression is range-checked: sums of one term and literals, where the term is
  a literal, a `clamp(x, lo, hi)` / `min(x, hi)` / `x & mask`, `X.length() - k` (X the array), a call of ch_bone (whose own definition must be the in-range form), XeBone / XeBone8
  (the S95 clamp of src\scene\xenos_vs_skin.glsl, -76..177 / -8..247), a loop counter with a literal bound, or a variable whose definition (`int v = ...`, ternaries as a union) is one of those.
The proved range must satisfy 0 <= index <= length - 1 for the DECLARED length (a symbolic `.length()` bound counts as the length). A mask or clamp limit past the declared
length FAILS (a 63-entry array indexed `& 255` is an offender). Parameters of ch_bone_point / ch_bone_turn are proved at every call site instead.
A site no rule proves must have a REVIEWED entry: (file, a regex that matches the WHOLE line, the reason): the line text is the key, so changing the line drops the review.
Exit 1 on any offender. --plant: three planted de-clamps (yMovieDefault, yCircleShadow, a `& 255` mask) must each give an offender in their file (exit 0 = all caught)."""
import argparse
import os
import re
import sys

EXT = ('.glsl', '.vert', '.frag', '.comp')
DIRS = (('src', 'renderer', 'shaders'), ('src', 'scene'))
PARAM_FUNCS = ('ch_bone_point', 'ch_bone_turn')
BONE_CLAMPS = {'XeBone': (-76, 177), 'XeBone8': (-8, 247)}

# (file, regex matching the whole stripped line, reason): sites the rules cannot see
REVIEWED = [
    ('rdr_2d_yuv.frag', r'vec2 plane_uv\(int p\) \{ return clamp\(in_uv \* fc\.plane\[p\]\.xy, fc\.plane\[p\]\.zw, fc\.plane\[p\]\.xy - fc\.plane\[p\]\.zw\); \}', 'p is 0..2 at the three call sites (plane_uv(0 / 1 / 2))'),
    ('rdr_uiquad.vert', r'vec3 position = vc\.Pos\[corner\]\.xyz;', 'corner comes from gl_VertexIndex of the 4-vertex quad; Pos and Uv are vec4[4]'),
    ('rdr_uiquad.vert', r'out_uv = vc\.Uv\[corner\]\.xy;', 'same'),
    ('rdr_uiquad_proj.vert', r'vec4 q = vec4\(vc\.Pos\[corner\]\.xyz, 1\.0\);', 'corner is 0..3 (list_of / strip_of of gl_VertexIndex 0..5); Pos is vec4[4]'),
    ('rdr_uiquad_proj.vert', r'out_uv = vc\.Uv\[corner\]\.xy;', 'same'),
    ('rdr_uiquad_vcol.vert', r'vec3 position = vc\.Pos\[corner\]\.xyz;', 'S105 A8: corner is 0..3 (corner_of of gl_VertexIndex 0..5); Pos and Col are vec4[4]'),
    ('rdr_uiquad_vcol.vert', r'out_color = vc\.Col\[corner\];', 'same'),
    ('rdr_uiquad_vcol_proj.vert', r'vec4 q = vec4\(vc\.Pos\[corner\]\.xyz, 1\.0\);', 'S105 A8: corner is 0..3 (list_of of gl_VertexIndex 0..5); Pos is vec4[4]'),
    ('rdr_uiquad_vcol_proj.vert', r'out_color = vc\.Col\[corner\];', 'same'),
    ('fx_skin.glsl', r'tint = mix\(vec3\(1\.0\), vc\.IdMapCol\.xyz, vc\.IdMapLvLst\[idSlot >> 2\]\[idSlot & 3\]\);', 'idSlot = packed & 7, so >> 2 is 0..1; IdMapLvLst[2]'),
    ('fx_skin.glsl', r'wrinkleLevel = vc\.WklLvLst\[wrinkleSlot >> 2\]\[wrinkleSlot & 3\];', 'wrinkleSlot is forced to <= 5 two lines above; >> 2 is 0..1; WklLvLst[2]'),
    ('yBlur.frag', r'sum \+= s\.rgb \* fc\.SampleWeights\[i \+ 1\]\.xyz;', 'the hole-fill loop is i < 12 and its block declares SampleWeights[13]'),
    ('yBlur.frag', r'total \+= fc\.SampleWeights\[i \+ 1\]\.x;', 'same'),
    ('yDownScale.frag', r'vec2 tapUV\(int i\) \{ return vTexCoord \+ fc\.g_avSampleOffsets\[i\]\.xy; \}', 'called with the loop counters of the declared taps only'),
    ('yRect.frag', r'vec4 group = fc\.GradationPointOffsets\[i >> 2\];', 'i < the declared point count (at most 8), >> 2 stays inside the vec4 groups'),
    ('yRect.frag', r'return mix\(fc\.GradationPointColors\[hit - 1\], fc\.GradationPointColors\[hit\], along\);', '1 <= hit <= count - 1 in this branch'),
    ('yRect.frag', r'return hit == 0 \? fc\.GradationPointColors\[0\] : fc\.GradationPointColors\[count - 1\];', 'count is the declared point count'),
    ('ySprite.frag', r'vec2 step = blurStep\(fc\.offset_weight\[tap\]\);', 'tap is the loop counter over the declared taps'),
    ('ySprite.frag', r'sum \+= \(texture\(textures\[0\], uv - step\) \+ texture\(textures\[0\], uv \+ step\)\) \* vec4\(fc\.offset_weight\[tap\]\.w\);', 'same'),
    ('ySprite.frag', r'vec4 group = fc\.GradationPointOffsets\[i >> 2\];', 'as yRect.frag'),
    ('ySprite.frag', r'if \(i >= 0\) return fc\.GradationPointColors\[i\];', 'guarded by i >= 0 and the point count'),
    ('ySprite.frag', r'return \(reg >= 3 && reg <= 10\) \? fc\.offset_weight\[reg - 3\] : vec4\(0\.0\);', 'guarded by reg >= 3 && reg <= 10 (offset_weight[8])'),
    ('rdr_plane_mask.vert', r'bool game_bool\(int n\) \{ return \(vc\.Bools\[n >> 7\]\[\(n >> 5\) & 3\] & \(1u << \(n & 31\)\)\) != 0u; \}', 'game_bool is called with literal bit numbers 1..4 only'),
    ('rdr_light_cubes.comp', r'fc\.PreLightK\[\d+\] = lc\.K\[base \+ \d+u\];', 'base = (workgroup z / 6) * 18 with the dispatch of 5 cubes: K[90] holds five 18-vec4 blocks; in range by the dispatch size (the planner dispatches 5 * 6 layers)'),
    ('yBlur.frag', r'vec4 s = texture\(textures\[0\], uv \+ fc\.SampleOffsets\[i\]\.xy\);', 'the hole-fill loop is i < 12; its block declares SampleOffsets[12]'),
    ('yBlur.frag', r'for \(int i = 0; i < TAP_COUNT; \+\+i\) sum \+= texture\(textures\[0\], uv \+ fc\.SampleOffsets\[i\]\.xy\) \* fc\.SampleWeights\[i\];', 'the block declares SampleOffsets / SampleWeights[TAP_COUNT] with the same macro the loop uses'),
    ('xenos_vs.glsl', r'uvec4 pair = xe_fetch\.fetch_constants\[fetch_constant >> 1\];', 'fetch_constant is a 5-bit fetch slot (0..31) of the 32-pair block'),
    ('xenos_vs_skin.glsl', r'bool XeBool\(int b\) \{ return \(xe_bool\.bool_constants\[b >> 7\]\[\(b >> 5\) & 3\] & \(1u << uint\(b & 31\)\)\) != 0u; \}', 'XeBool is called with the microcode bool index, 0..255, of the 2-uvec4 block'),
    ('fx_common.glsl', r'return \(bools\[n >> 7\]\[\(n >> 5\) & 3\] & \(1u << \(n & 31\)\)\) != 0u;', 'fx_bool(bools, n) is called with literal bit numbers below 256 only'),
]

DECL = re.compile(r'\b(?:u?vec[234]|ivec[234]|mat[234]|float|u?int)\s+(\w+)\s*\[\s*(\w+)\s*\]\s*;')
SITE = re.compile(r'\b(?:\w+\.(\w+)|(CH_BONES))\[')


def split_top(expr, ch):
    parts, depth, cur = [], 0, ''
    for c in expr:
        if c in '([':
            depth += 1
        elif c in ')]':
            depth -= 1
        if c == ch and depth == 0:
            parts.append(cur)
            cur = ''
        else:
            cur += c
    parts.append(cur)
    return parts


def matching(text, i):
    """index of the bracket closing the one at text[i]"""
    opener = text[i]
    closer = {'(': ')', '[': ']'}[opener]
    depth = 0
    for j in range(i, len(text)):
        if text[j] == opener:
            depth += 1
        elif text[j] == closer:
            depth -= 1
            if depth == 0:
                return j
    return -1


class Ctx:
    def __init__(self, lines, lens, loops, ch_ok):
        self.lines, self.lens, self.loops, self.ch_ok = lines, lens, loops, ch_ok


def num(s):
    s = s.strip().rstrip('uU')
    return int(s, 0) if re.fullmatch(r'-?(0x[0-9a-fA-F]+|\d+)', s) else None


def rng(expr, ctx, line_no, arr, depth=0):
    """-> (lo, hi) as (const, uses_length) pairs, or None. value = const + (L if uses_length)."""
    e = expr.strip()
    if depth > 6 or not e:
        return None
    while e.startswith('(') and matching(e, 0) == len(e) - 1:
        e = e[1:-1].strip()
    n = num(e)
    if n is not None:
        return ((n, 0), (n, 0))
    # sum of terms
    terms = split_top(e.replace(' - ', ' + -'), '+')
    if len(terms) > 1:
        lo, hi = (0, 0), (0, 0)
        for t in terms:
            r = rng(t, ctx, line_no, arr, depth + 1)
            if r is None:
                return None
            lo, hi = (lo[0] + r[0][0], lo[1] + r[0][1]), (hi[0] + r[1][0], hi[1] + r[1][1])
        return lo, hi
    m = re.fullmatch(r'([\w.]+)\.length\(\)', e)
    if m:
        return ((0, 1), (0, 1)) if m.group(1) in (arr, 'CH_BONES') or True else None
    m = re.fullmatch(r'(clamp|min)\s*\(', e[:e.find('(') + 1].strip()) if '(' in e else None
    if m and e.endswith(')') and matching(e, e.find('(')) == len(e) - 1:
        args = split_top(e[e.find('(') + 1:-1], ',')
        if m.group(1) == 'clamp' and len(args) == 3:
            lo, hi = rng(args[1], ctx, line_no, arr, depth + 1), rng(args[2], ctx, line_no, arr, depth + 1)
            return (lo[0], hi[1]) if lo and hi else None
        if m.group(1) == 'min' and len(args) == 2:
            for a, b in ((args[0], args[1]), (args[1], args[0])):
                h = rng(b, ctx, line_no, arr, depth + 1)
                if h and num(a) is None:
                    return (None, h[1])   # lower end unknown unless the other side is proved
            return None
    m = re.fullmatch(r'(.+?)\s*&\s*(\w+)', e)
    if m and num(m.group(2)) is not None and num(m.group(2)) >= 0:
        return ((0, 0), (num(m.group(2)), 0))
    m = re.fullmatch(r'(XeBone8?)\s*\(.*\)', e)
    if m:
        a, b = BONE_CLAMPS[m.group(1)]
        return ((a, 0), (b, 0))
    if re.fullmatch(r'ch_bone\s*\(.*\)', e):
        return ((-1, 0), (-3, 1)) if ctx.ch_ok else None   # -1: no bone; the readers guard it (checked below)
    if '?' in e:   # ternary: union of the two branches (the condition may add a guard, which only narrows)
        q = split_top(e, '?')
        if len(q) == 2:
            c = split_top(q[1], ':')
            if len(c) == 2:
                a, b = rng(c[0], ctx, line_no, arr, depth + 1), rng(c[1], ctx, line_no, arr, depth + 1)
                if a and b and a[0] and b[0] and a[1] and b[1]:
                    key = lambda v: v[0] + 1000 * v[1]   # the length is at least 3 here: compare c + 1000 * l
                    return (min(a[0], b[0], key=key), max(a[1], b[1], key=key))
        return None
    if re.fullmatch(r'\w+', e):
        if e in ctx.loops.get(line_no, {}):
            return ((0, 0), (ctx.loops[line_no][e] - 1, 0))
        for k in range(line_no - 1, -1, -1):   # the nearest earlier definition `int e = RHS;`
            m = re.search(r'\b(?:int|uint)\s+' + e + r'\s*=\s*([^;]+);', ctx.lines[k])
            if m:
                return rng(m.group(1), ctx, k, arr, depth + 1)
            if re.search(r'\b(?:int|uint|float)\s+' + e + r'\b', ctx.lines[k]) and '=' not in ctx.lines[k].split(e, 1)[1][:3]:
                return None
        return None
    return None


def proved(r, length, sym):
    """r = (lo, hi) pairs; length = declared number or None (symbolic)"""
    if r is None or r[0] is None or r[1] is None:
        return False
    (lc, lu), (hc, hu) = r
    if lu:
        return False
    if lc < 0:
        return False
    if hu:   # c + L <= L - 1  <=>  c <= -1
        return hc <= -1
    return length is not None and hc <= length - 1


def lines_of(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        return f.read().splitlines()


def scan_text(rel, text, ch_ok, fn_calls):
    """-> list of (line_no, site, ok, reason, line)"""
    ls = text.splitlines()
    fn = os.path.basename(rel)
    lens = {}
    for l in ls:
        for m in DECL.finditer(l):
            lens[m.group(1)] = num(m.group(2))
    loops = {}
    for i, l in enumerate(ls):
        for m in re.finditer(r'for\s*\(\s*(?:int|uint)\s+(\w+)\s*=\s*0u?\s*;\s*\1\s*<\s*(\d+)', l):
            for j in range(i, min(len(ls), i + 40)):
                loops.setdefault(j, {})[m.group(1)] = int(m.group(2))
    ctx = Ctx(ls, lens, loops, ch_ok)
    out = []
    for i, l in enumerate(ls):
        s = l.strip()
        if s.startswith('//'):
            continue
        code = l.split('//')[0]
        for m in SITE.finditer(code):
            arr = m.group(1) or m.group(2)
            if m.group(2) == 'CH_BONES':
                length = None
            else:
                if arr not in lens:
                    continue    # not a sized array of this file (a runtime array, a member of something else)
                length = lens[arr]
            b = m.end() - 1
            e = matching(code, b)
            if e < 0:
                continue
            idx = code[b + 1:e].strip()
            if num(idx) is not None:
                continue
            site = code[m.start():e + 1]
            r = rng(idx, ctx, i, arr)
            ok, why = proved(r, length, arr == 'CH_BONES'), 'range proved'
            if not ok and re.fullmatch(r'\w+( \+ \d+)?', idx) and re.search(r'ch_bone_(point|turn)\s*\(\s*int\s+' + re.escape(idx.split()[0]) + r'\b', ' '.join(ls[max(0, i - 2):i + 1])):
                ok, why = fn_calls and re.search(r'if \(' + re.escape(idx.split()[0]) + r' < 0\) return vec3\(0\.0\);', ' '.join(ls[max(0, i - 2):i + 1])) is not None, 'a parameter of ch_bone_point / ch_bone_turn: every call site proved (%s)' % ('yes' if fn_calls else 'NO')
            if not ok:
                for f, pat, reason in REVIEWED:
                    if f == fn and re.fullmatch(pat, s):
                        ok, why = True, 'reviewed: ' + reason
                        break
            out.append((i + 1, site, ok, why if ok else None, s))
    return out


def ch_bone_ok(files):
    for text in files.values():
        for l in text.splitlines():
            if re.match(r'\s*int ch_bone\(float id\)', l):
                return bool(re.search(r'int b = 3 \* int\(id\); return \(b >= 0 && b <= CH_BONES\.length\(\) - 3\) \? b : -1;', l))
    return False


def param_calls_ok(files, ch_ok):
    """every call of ch_bone_point / ch_bone_turn: its first argument is proved to be in [0, L - 3]"""
    bad = []
    for rel, text in files.items():
        ls = text.splitlines()
        loops = {}
        ctx = Ctx(ls, {}, loops, ch_ok)
        for i, l in enumerate(ls):
            code = l.split('//')[0]
            for m in re.finditer(r'\b(ch_bone_point|ch_bone_turn)\s*\(', code):
                if re.search(r'\b(vec3|int)\s+$', code[:m.start()]):
                    continue   # the definition
                a = split_top(code[m.end():matching(code, m.end() - 1)], ',')[0]
                r = rng(a, ctx, i, 'CH_BONES')
                if not (r and r[0] and r[1] and r[0][1] == 0 and r[0][0] >= -1 and ((r[1][1] == 1 and r[1][0] <= -3) or (r[1][1] == 0 and r[1][0] <= 0))):   # a literal 0 is bone 0 of a palette of at least 3 rows
                    bad.append('%s:%d ch_bone_* argument %r not proved' % (rel, i + 1, a.strip()))
    return bad


def load(root, override=None):
    files = {}
    for d in DIRS:
        base = os.path.join(root, *d)
        for dp, dn, fns in os.walk(base):
            if os.sep + 'spv' in dp:
                continue
            for fn in fns:
                if fn.endswith(EXT):
                    path = os.path.join(dp, fn)
                    rel = os.path.relpath(path, root).replace('\\', '/')
                    t = '\n'.join(lines_of(path))
                    files[rel] = override(rel, t) if override else t
    return files


def scan(root, override=None):
    files = load(root, override)
    ch_ok = ch_bone_ok(files)
    bad_calls = param_calls_ok(files, ch_ok)
    results = []
    for rel, text in sorted(files.items()):
        for r in scan_text(rel, text, ch_ok, not bad_calls):
            results.append((rel,) + r)
    return results, ch_ok, bad_calls


PLANTS = [
    ('fx/yMovieDefault.vert', 'clamp(3 * int(inBoneId), 0, vc.WorldMatrixArray.length() - 3)', '3 * int(inBoneId)'),
    ('fx/yCircleShadow.vert', 'int first = clamp(int(inPointIds.x), 0, vc.WorldPosArray.length() - 1);', 'int first = int(inPointIds.x);'),
    ('fx/fx_character.glsl', 'int b = 3 * int(id); return (b >= 0 && b <= CH_BONES.length() - 3) ? b : -1;', 'return 3 * int(id);'),
    ('fx/fx_character.glsl', 'vec3 ch_bone_point(int b, vec4 p) { if (b < 0) return vec3(0.0); ', 'vec3 ch_bone_point(int b, vec4 p) { '),
    ('fx/yPlaneShadowMap.vert', 'int b = clamp(3 * id, 0, vc.WorldMatrixArray.length() - 3);', 'int b = (3 * id) & 255;'),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--plant', action='store_true')
    ap.add_argument('--gate', action='store_true', help='the lint, then the plants (the check)')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    if a.gate:
        a.plant = False
        rc = run(a, root)
        a.plant = True
        return rc | run(a, root)
    return run(a, root)


def run(a, root):
    if a.plant:
        base, ch_ok, bad = scan(root)
        base_bad = {(r[0], r[1]) for r in base if not r[3]}
        for tail, good, planted in PLANTS:
            applied = []

            def ov(rel, t, tail=tail, good=good, planted=planted):
                if rel.endswith('src/renderer/shaders/' + tail):
                    if good not in t:
                        print('PLANT DID NOT APPLY: %r is not in %s' % (good, rel))
                        sys.exit(2)
                    applied.append(rel)
                    return t.replace(good, planted)
                return t
            res, ch_p, bc_p = scan(root, ov)
            new_bad = [r for r in res if not r[3] and r[0].endswith(tail) and (r[0], r[1]) not in base_bad]
            if not applied or not (new_bad or (tail.endswith('fx_character.glsl') and not ch_p)):
                print('PLANT NOT CAUGHT: %s -> %s' % (good, planted))
                return 1
            print('PLANT CAUGHT: %s (%d new offender site(s), ch_bone form ok: %s)' % (tail, len(new_bad), ch_p))
        return 0
    res, ch_ok, bad_calls = scan(root)
    bad = [r for r in res if not r[3]]
    if not ch_ok:
        print('OFFENDER ch_bone is not in the in-range form (b >= 0 && b <= CH_BONES.length() - 3) ? b : 0')
    for b in bad_calls:
        print('OFFENDER ' + b)
    print('const_index_lint: %d dynamic-index sites in %d files, %d proved or reviewed, %d offenders%s' % (
        len(res), len({r[0] for r in res}), len(res) - len(bad), len(bad) + len(bad_calls) + (0 if ch_ok else 1), ''))
    for rel, n, site, ok, why, line in (res if a.list else bad):
        print('  %s %s:%d  %s   [%s]' % ('ok ' if ok else 'BAD', rel, n, site, why or line))
    return 1 if (bad or bad_calls or not ch_ok) else 0


if __name__ == '__main__':
    sys.exit(main())
