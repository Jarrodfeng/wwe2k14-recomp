r"""q targetfmt: per render-target ROLE, the format the GAME blends into and keeps beside OURS (S111 TARGETFMT; the code diff's
target-format extractor, docs/research/s111/WHOLEDIFF_PLAN.md A.5.2).

    py tools/q/q.py targetfmt [--role R] [--field F] [--flag DIFF ...] [--facts] [--json] [--ref REF]

One row per (role, field). Fields (the code-diff fact fields):
  target_format  the colour format the blend writes into. GAME: the format word (r5) of each surface create `sub_82CA88E0` the role
                 names, as an EDRAM colour format (xenos.h ColorRenderTargetFormat). OURS: the rdr::Format of the image the role's
                 function makes (`VAR.format = rdr::Format::X` inside it).
  blend_space    where the blend runs, by each side's format rule. The 360: a k_8_8_8_8_GAMMA target blends the shader's (linear) values
                 and gamma-encodes after the blend (the SDK says so where it cannot: spirv_translator_rb.cpp
                 SpirvShaderTranslator::CompleteFragmentShaderInMain "this is incorrect, since it must be done after blending on the Xbox
                 360"); every other format blends the values as written. OURS: an *_SRGB rdr::Format blends linear and encodes after;
                 every other format (all of today's) blends the values as written.
  precision      what the next reader gets: GAME = the format word (r8) of the resolve destinations `sub_82CA87C0` the role names (the
                 texture the surface is resolved into), else the surface's own; OURS = the image's format, narrowed by a quantise step
                 a shader of the role applies: `floor(V * N + 0.5) / N` or `round(V * N) / N` in src/renderer/shaders/fx/<E>.frag under
                 the define(s) of a fragment variant of <E>.fx.json, counted only when the role's file looks that variant up
                 (`FindVariant("<E>_<variant>_fragment")`) and reads the id it stores again (LIQUID4BIT's copy_prev_q4: N = 15 = 4 bits).
Format words: low 6 bits = the Xenos texture format (xenos.h TextureFormat), bits 9-16 = the X / Y / Z / W sign fields (2 bits each; 3 =
gamma: docs/renderer/GAME_RULES_TEXTURES_FRAME.md T16, claim db/claims/s111_SHINEFETCH.json 0x83630E64). A k_8_8_8_8 surface with gamma
signs on X, Y and Z is k_8_8_8_8_GAMMA [L: the XDK's sRGB format form; no create the game makes carries it]; k_8_8_8_8_GAMMA_EDRAM (62)
and k_2_10_10_10_FLOAT_EDRAM (63) name their target format themselves. A format with no colour target form (k_4_4_4_4, k_8) is a
resolve destination only.
Verdicts (the code-diff set): DIFF (one value each side, they differ), GAME-ONLY (no ours image), OURS-ONLY (no game site), UNSETTLED (a
site with no constant / an undecoded word, or several values on a side whose pairs do not all agree: the role row does not say which
surface is which image), DEPARTED (a DIFF with a `// DEPARTS:` marker on our format line or the line above, read as q codediff reads
it), OK. Comparison: kind (unorm / snorm / fixed / half / float / float7e3) and bits per channel; ours with fewer channels than the
game is a target_format DIFF, more is OK (said in the row).
The role table (ROLES) pairs the game's create sites with our function by role; each row carries its evidence and its pair mark
([C] the doc / note read the role, [L] the doc names the creator but not which surface plays the role). The game side is read from
the q index (`q args` values at the sites: tools/q/qargs.py collect / Folder), the same at any --ref; ours at the working tree or
--ref (git show). Every create site of the two creators no role names is listed (coverage), never judged.
Exit 0; 2 on a PROBLEM (a role's function or variable not found in our code, a site missing from the index).
"""
import argparse
import fnmatch
import json
import math
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import qconstdiff as CD   # noqa: E402  (_mask, _match)
import qostate as O       # noqa: E402  (default_reader: the tree at a ref)

REPO = CD.REPO
FX_DIR = 'src/renderer/shaders/fx'
SURFACE_FN, TEXTURE_FN = 0x82CA88E0, 0x82CA87C0       # the two creators (docs/systems/render_targets.md "The two creators")
SURFACE_REG, TEXTURE_REG = 'r5', 'r8'
FIELDS = ('target_format', 'blend_space', 'precision')
VERDICTS = ('DIFF', 'GAME-ONLY', 'UNSETTLED', 'OURS-ONLY', 'DEPARTED', 'OK')
RANK = {v: i for i, v in enumerate(VERDICTS)}
# selftest only: a planted twin the selftest must see fail ('no_dest' precision from the surface only, 'no_gamma' the sign bits
# ignored, 'no_quant' our quantise step not read, 'quant_unused' a quantising variant counted without its lookup)
PLANT = os.environ.get('WWE_QTARGETFMT_PLANT', '')

# Xenos texture format number -> (name, kind, bits per colour channel, channels, colour target form or None = resolve destination only)
XFMT = {
    2: ('k_8', 'unorm', 8, 1, None),
    6: ('k_8_8_8_8', 'unorm', 8, 4, 'k_8_8_8_8'),
    7: ('k_2_10_10_10', 'unorm', 10, 4, 'k_2_10_10_10'),
    10: ('k_8_8', 'unorm', 8, 2, None),
    13: ('k_16_16_EDRAM', 'fixed', 16, 2, 'k_16_16'),
    15: ('k_4_4_4_4', 'unorm', 4, 4, None),
    21: ('k_16_16_16_16_EDRAM', 'fixed', 16, 4, 'k_16_16_16_16'),
    22: ('k_24_8', 'depth', 24, 1, None),
    23: ('k_24_8_FLOAT', 'depth', 24, 1, None),
    24: ('k_16', 'unorm', 16, 1, None),
    25: ('k_16_16', 'unorm', 16, 2, None),
    26: ('k_16_16_16_16', 'unorm', 16, 4, None),
    30: ('k_16_FLOAT', 'half', 16, 1, None),
    31: ('k_16_16_FLOAT', 'half', 16, 2, 'k_16_16_FLOAT'),
    32: ('k_16_16_16_16_FLOAT', 'half', 16, 4, 'k_16_16_16_16_FLOAT'),
    36: ('k_32_FLOAT', 'float', 32, 1, 'k_32_FLOAT'),
    37: ('k_32_32_FLOAT', 'float', 32, 2, 'k_32_32_FLOAT'),
    38: ('k_32_32_32_32_FLOAT', 'float', 32, 4, None),
    62: ('k_8_8_8_8_GAMMA_EDRAM', 'unorm', 8, 4, 'k_8_8_8_8_GAMMA'),
    63: ('k_2_10_10_10_FLOAT_EDRAM', 'float7e3', 10, 4, 'k_2_10_10_10_FLOAT'),
}
GAMMA_RT = ('k_8_8_8_8_GAMMA',)
# rdr::Format (src/renderer/api.h) -> (kind, bits, channels)
RFMT = {
    'RGBA8': ('unorm', 8, 4), 'BGRA8': ('unorm', 8, 4), 'RGBA8_SNORM': ('snorm', 8, 4), 'R8': ('unorm', 8, 1), 'R8_SNORM': ('snorm', 8, 1),
    'RG8': ('unorm', 8, 2), 'RGBA4': ('unorm', 4, 4), 'R5G6B5': ('unorm', 5, 3), 'A1RGB5': ('unorm', 5, 4), 'RGB10A2': ('unorm', 10, 4),
    'RGBA16_SNORM': ('snorm', 16, 4), 'RGBA16_UNORM': ('unorm', 16, 4), 'RG16_UNORM': ('unorm', 16, 2), 'RG16_SNORM': ('snorm', 16, 2),
    'R16_UNORM': ('unorm', 16, 1), 'RGBA16F': ('half', 16, 4), 'RG16F': ('half', 16, 2), 'R16F': ('half', 16, 1),
    'RGBA32F': ('float', 32, 4), 'RG32F': ('float', 32, 2), 'R32F': ('float', 32, 1), 'R32_UINT': ('uint', 32, 1), 'Depth': ('depth', 24, 1),
}

# The role table. surface / dest = the call sites of sub_82CA88E0 / sub_82CA87C0 (q args); ours = (file, function, the ImageDesc variable).
ROLES = [
    dict(role='scene_colour', surface=[0x82C22C18], dest=[],
         ours=[('src/renderer_scene/front_end.cpp', 'FrontEnd::EnsureTargets', 'c')], fx=[], pair='C',
         why='render_targets.md M14 (sub_82C22AF8 colour, scene object +272); post_effects.md section 13 (both 8-bit); its resolve copy '
             '[B948]+4 is made through sub_82BCB910, whose word q args does not show: precision is the surface\'s'),
    dict(role='sweat_map', surface=[0x8279992C], dest=[0x827999A8],
         ours=[('src/renderer_scene/live_map_sweat.cpp', 'LiveMapsHook::RecordSweat', 'c')], fx=['yLiquidMap'], pair='C',
         why='render_targets.md M6 (sub_82799890 "yliquidmap"); LIQUID4BIT.md section 1 (the surface is 8-bit, the map texture '
             '[map+248] k_4_4_4_4, resolved into every frame by sub_82799B60)'),
    dict(role='plane_shadow', surface=[0x827EE788, 0x827EE800, 0x827EE58C], dest=[0x827EE768, 0x827EE7E0],
         ours=[('src/renderer_scene/live_map_plane.cpp', 'LiveMapsHook::RecordPlane', 'c')], fx=[], pair='C',
         why='render_targets.md M1-M3 (sub_827EE5C0 512x512 mask + 256x256, sub_827EE480 the blurs; which surface is M1 / M2 / M3 [L])'),
    dict(role='shadow_maps', surface=[0x827E8E00, 0x827E8ED0, 0x827E8FC8, 0x827E90B4], dest=[],
         ours=[('src/renderer_scene/live_maps.cpp', 'LiveMapsHook::EnsureImages', 'c')], fx=[], pair='L',
         why='render_targets.md M8-M11 / M7 (sub_827E8CC8 seven creates; surface-to-role [U]); ours makes the shadow map and the '
             'props / circles map there'),
    dict(role='rim_map', surface=[0x827EA644, 0x827EA72C], dest=[],
         ours=[('src/renderer_scene/live_map_rim.cpp', 'LiveMapsHook::RecordRim', 'c')], fx=[], pair='C',
         why='render_targets.md M12 (sub_827EA4F0, S101 lane 86)'),
    dict(role='light_strip', surface=[0x82C203EC], dest=[0x82BD4764],
         ours=[('src/renderer_scene/live_map_prelight.cpp', 'LiveMapsHook::RecordPreLight', 'c')], fx=[], pair='C',
         why='render_targets.md M13 (sub_82C202D0 "yprelight", 384x64 strips copied into 64x64 cube textures sub_82BD4720)'),
    dict(role='crowd_cards', surface=[0x827FD440], dest=[0x827FD418],
         ours=[('src/renderer_scene/live_crowd.cpp', 'CardImage', 'd')], fx=[], pair='L',
         why='render_targets.md M4 (sub_827FD3C8 crowd thumbnails; role [L])'),
    dict(role='crowd_atlas', surface=[0x825BB56C], dest=[0x825BB4D0],
         ours=[('src/renderer_scene/live_crowd.cpp', 'EnsureAtlas', 'd')], fx=[], pair='L',
         why='render_targets.md M5 (sub_825BB398 512x512 atlas; role [L])'),
    dict(role='post_luminance', surface=[0x82C1C334, 0x82C1C3A0], dest=[0x82C1C318, 0x82C1C35C, 0x82C1C384],
         ours=[('src/renderer_scene/post_pass.cpp', 'PostPass::MakeF', 'd')], fx=[], pair='L',
         why='render_targets.md HDR chain (sub_82C1C278 "yGlareEtc": its 64x64 / 16x16 k_32_FLOAT creates; that they are the '
             'luminance measure [L]) beside our luminance pyramid'),
    dict(role='post_chain', surface=[0x82C1C40C, 0x82C1C470, 0x82C1C4E4, 0x82C2B1D4, 0x82C2976C],
         dest=[0x82C1C3E0, 0x82C1C444, 0x82C1C4B0, 0x82C29798],
         ours=[('src/renderer_scene/post_pass.cpp', 'PostPass::Make', 'd')], fx=[], pair='L',
         why='render_targets.md HDR chain (sub_82C1C278 eighth-size), DoF (sub_82C2B128 quarter), halation (sub_82C296C0); which of '
             'ours each feeds [L]'),
    dict(role='frame_buffer', surface=[0x82C22C18], dest=[0x827DC8C8, 0x82C229DC],
         ours=[('src/renderer_host/scene_host.cpp', 'SceneHost::MakeF', 'd')], fx=[], pair='L',
         why='render_targets.md frame_buffer; FBFMT.md: the present (present_object_texture sub_82C227E8) resolves into and swaps the '
             'scene object texture sub_82C22980 makes, from the scene colour surface sub_82C22AF8 makes; sub_827DC770 the persist-display '
             'copy. engine_display_ctor sub_82BA8018 +3F4 (k_16_16_16_16) is a dead branch (+332 = 0); which surface the capture\'s '
             'frame image is [L]'),
]


class TargetFmtError(Exception):
    pass


def line_of(text, pos):
    return text.count('\n', 0, pos) + 1


# ------------------------------------------------------------------ the game: format words
def decode(word):
    """a D3D format word -> {word, fmt, name, kind, bits, ch, rt, gamma, signs} (name None = an undecoded format number)"""
    f = word & 0x3F
    signs = [(word >> (9 + 2 * i)) & 3 for i in range(4)]
    row = XFMT.get(f)
    d = {'word': f'0x{word:08X}', 'fmt': f, 'signs': signs, 'name': None, 'kind': None, 'bits': None, 'ch': None, 'rt': None, 'gamma': False}
    if row is None:
        return d
    name, kind, bits, ch, rt = row
    gamma = rt in GAMMA_RT
    if f == 6 and PLANT != 'no_gamma' and all(s == 3 for s in signs[:3]):
        rt, gamma, name = 'k_8_8_8_8_GAMMA', True, 'k_8_8_8_8 (gamma signs)'
    elif kind == 'unorm' and all(s == 1 for s in signs[:ch if ch < 4 else 3]):
        kind = 'snorm'
    d.update(name=name, kind=kind, bits=bits, ch=ch, rt=rt, gamma=gamma)
    return d


def default_sites(con):
    """-> {site: {fn, who, creator, reg, value, text}} for every call of the two creators (q args)"""
    import qargs
    folder = qargs.Folder(con, None)
    out = {}
    for f, reg, creator in ((SURFACE_FN, SURFACE_REG, 'surface'), (TEXTURE_FN, TEXTURE_REG, 'texture')):
        _reads, rows = qargs.collect(con, f)
        for r in rows:
            v, text = None, r['why'] or 'no value'
            if r['regs'] and reg in r['regs']:
                kind, key, disp = folder.fold(r['regs'][reg], reg)
                text = disp
                if kind == 'const':
                    v = int(key, 0) & 0xFFFFFFFF
            size = ''
            if r['regs']:
                size = ' x '.join(str(r['regs'].get(x, '?')) for x in (('r3', 'r4')))
            out[r['site']] = {'fn': f"sub_{r['src']:08X}", 'who': r['who'], 'creator': creator, 'reg': reg, 'value': v, 'text': text, 'size': size}
    return out


# ------------------------------------------------------------------ ours: the role's function, its format, the quantise step
def find_function(text, name):
    """-> (masked text, body start, body end) of the definition of NAME (`A::B` or an in-class `B`), or None. A definition is the name,
    its parameter list and a `{` before any `;`."""
    ms = CD._mask(text)
    for m in re.finditer(r'(?<![\w:.>])' + re.escape(name) + r'\s*\(', ms):
        close = CD._match(ms, m.end() - 1, '(', ')')
        k = close
        while k < len(ms) and ms[k] not in '{;':
            k += 1
        if k < len(ms) and ms[k] == '{':
            head = ms[close:k]
            if re.fullmatch(r'[\s\w:&*,()<>\[\].]*', head) and not re.search(r'[=+\-/%!?]', head):
                return ms, k, CD._match(ms, k, '{', '}')
    return None


def ours_formats(read, file, fn, var):
    """-> ([{format, at, line}], problem or None): every `VAR.format = ...` in FN's body (a non-literal value: format None)"""
    try:
        text = read(file)
    except (OSError, O.OstateError) as e:
        return [], f'{file}: cannot be read ({e})'
    hit = find_function(text, fn)
    if hit is None:
        return [], f'{file}: no definition of {fn}'
    ms, a, b = hit
    out = []
    for m in re.finditer(r'(?<![\w.])' + re.escape(var) + r'\s*\.\s*format\s*=\s*([^;,]+?)\s*[;,]', ms[a:b]):
        val = m.group(1).strip()
        lit = re.fullmatch(r'rdr::Format::(\w+)', val)
        ln = line_of(ms, a + m.start())
        out.append({'format': lit.group(1) if lit else None, 'expr': val, 'at': f'{file}:{ln}', 'line': ln})
    if not out:
        return [], f'{file}: {fn} sets no {var}.format'
    return out, None


QUANT_RE = re.compile(r'(?:floor\(\s*[\w.\[\]]+\s*\*\s*(?P<n1>\d+(?:\.\d*)?)\s*\+\s*0?\.5\s*\)|round\(\s*[\w.\[\]]+\s*\*\s*(?P<n2>\d+(?:\.\d*)?)\s*\))'
                      r'\s*/\s*(?P<d>\d+(?:\.\d*)?)')


def quant_lines(frag):
    """-> [(line, steps N, requirement set)] for each quantise expression of a fragment source; the requirement = the defines its
    #if / #ifdef / #elif branch needs (`!D` = must not be defined; an #else needs none of its chain's earlier defines)"""
    out, stack = [], []
    for i, raw in enumerate(CD._mask(frag).split('\n'), 1):
        s = raw.strip()
        m = re.match(r'#\s*(ifdef|ifndef|if|elif|else|endif)\b(.*)', s)
        if m:
            kw, rest = m.group(1), m.group(2)
            if kw == 'ifdef':
                stack.append({'need': {rest.split()[0]}, 'prev': {rest.split()[0]}})
            elif kw == 'ifndef':
                stack.append({'need': {'!' + rest.split()[0]}, 'prev': set()})
            elif kw == 'if':
                names = set(re.findall(r'defined\s*\(?\s*(\w+)', rest))
                stack.append({'need': names, 'prev': set(names)})
            elif kw == 'elif' and stack:
                names = set(re.findall(r'defined\s*\(?\s*(\w+)', rest))
                top = stack[-1]
                top['need'] = names | {'!' + p for p in top['prev']}
                top['prev'] |= names
            elif kw == 'else' and stack:
                top = stack[-1]
                top['need'] = {'!' + p for p in top['prev']}
            elif kw == 'endif' and stack:
                stack.pop()
            continue
        for q in QUANT_RE.finditer(raw):
            n = q.group('n1') or q.group('n2')
            if float(n) != float(q.group('d')):
                continue
            need = set()
            for e in stack:
                need |= e['need']
            out.append((i, int(float(n)), need))
    return out


def ours_quant(read, role):
    """-> ([{steps, bits, variant, at, use}], problems): the quantise steps of the role's effects that its file looks up and uses"""
    if PLANT == 'no_quant':
        return [], []
    files = sorted({f for f, _fn, _v in role['ours']})
    texts = {}
    for f in files:
        try:
            texts[f] = CD._mask(read(f))
        except (OSError, O.OstateError):
            texts[f] = ''
    out, problems = [], []
    for eff in role['fx']:
        try:
            frag = read(f'{FX_DIR}/{eff}.frag')
            reg = json.loads(read(f'{FX_DIR}/{eff}.fx.json'))
        except (OSError, O.OstateError, ValueError) as e:
            problems.append(f'{FX_DIR}/{eff}: the fragment source / registry cannot be read ({e})')
            continue
        qs = quant_lines(frag)
        for v in reg.get('fragment', []):
            defs = set(v.get('defines', []))
            for ln, n, need in qs:
                if not all((x[1:] not in defs) if x.startswith('!') else (x in defs) for x in need):
                    continue
                full = f"{eff}_{v['name']}_fragment"
                for f, t in texts.items():
                    for m in re.finditer(r'(?P<lv>[\w.\->\[\]]+)\s*=\s*[\w:]*FindVariant\(\s*"' + re.escape(full) + r'"\s*\)', t):
                        last = re.split(r'\.|->', m.group('lv'))[-1]
                        aln = line_of(t, m.start())
                        uses = [line_of(tt, u.start()) for ff, tt in texts.items() for u in re.finditer(r'\b' + re.escape(last) + r'\b', tt)
                                if not (ff == f and line_of(tt, u.start()) == aln)]
                        if not uses and PLANT != 'quant_unused':
                            continue
                        bits = math.log2(n + 1)
                        out.append({'steps': n, 'bits': int(bits) if bits.is_integer() else None, 'variant': full,
                                    'at': f'{f}:{aln}', 'shader': f'{FX_DIR}/{eff}.frag:{ln}', 'use': uses[:3]})
    return out, problems


# ------------------------------------------------------------------ the join
def g_val(field, d, dests):
    if field == 'target_format':
        return None if d['rt'] is None else {'name': d['rt'], 'kind': d['kind'], 'bits': d['bits'], 'ch': d['ch']}
    if field == 'blend_space':
        return None if d['rt'] is None else ('linear, gamma after the blend' if d['gamma'] else 'as written')
    return {'name': d['name'], 'kind': d['kind'], 'bits': d['bits'], 'ch': d['ch']}


def o_val(field, fmt, quant):
    row = RFMT.get(fmt)
    srgb = 'SRGB' in fmt.upper()
    if row is None and srgb:
        row = ('unorm', 8, 4)
    if row is None:
        return None
    kind, bits, ch = row
    if field == 'target_format':
        return {'name': fmt, 'kind': kind, 'bits': bits, 'ch': ch}
    if field == 'blend_space':
        return 'linear, sRGB after the blend' if srgb else 'as written'
    if quant and kind in ('unorm', 'snorm', 'half', 'float') and quant[0]['bits'] is not None and quant[0]['bits'] < bits:
        return {'name': f"{fmt} + {quant[0]['steps']}-step quantise", 'kind': 'unorm', 'bits': quant[0]['bits'], 'ch': ch}
    return {'name': fmt, 'kind': kind, 'bits': bits, 'ch': ch}


def same(field, g, o):
    """-> (equal, note)"""
    if field == 'blend_space':
        return g.split(',')[0] == o.split(',')[0], ''
    if (g['kind'], g['bits']) != (o['kind'], o['bits']):
        more = (o['bits'] or 0) > (g['bits'] or 0)
        return False, 'ours keeps more' if more else 'ours keeps less'
    if field == 'target_format' and o['ch'] < g['ch']:
        return False, f"ours has {o['ch']} channel(s), the game {g['ch']}"
    if o['ch'] > g['ch']:
        return True, f"ours holds {o['ch']} channels where the game keeps {g['ch']}"
    return True, ''


def show(field, v):
    if v is None:
        return '?'
    if field == 'blend_space':
        return v
    return f"{v['name']} ({v['kind']}{v['bits']} x{v['ch']})"


def judge(field, gvals, ovals, g_unknown, o_unknown, marks=None, o_ats=()):
    """-> (verdict, detail)"""
    if not gvals and not g_unknown:
        return 'OURS-ONLY', 'no game site'
    if not ovals and not o_unknown:
        return 'GAME-ONLY', 'no ours image'
    if g_unknown or o_unknown:
        return 'UNSETTLED', '; '.join(g_unknown + o_unknown)
    gs = {json.dumps(v, sort_keys=True): v for v in gvals}
    os_ = {json.dumps(v, sort_keys=True): v for v in ovals}
    pairs = [(g, o) + same(field, g, o) for g in gs.values() for o in os_.values()]
    notes = sorted({p[3] for p in pairs if p[3]})
    if all(p[2] for p in pairs):
        return 'OK', '; '.join(notes)
    if len(gs) > 1 or len(os_) > 1:
        return 'UNSETTLED', (f'game {len(gs)} values, ours {len(os_)}: the role row does not say which surface is which image; '
                             + '; '.join(notes))
    detail = '; '.join(notes)
    for at in o_ats:
        if marks is not None:
            mk, why = marks.at(at)
            if mk:
                return 'DEPARTED', f"{mk['class']} {mk['file']}:{mk['ref']}: {mk['why']}"
            if why:
                detail += f'; DEPARTS marker at {at} refused: {why}'
    return 'DIFF', detail


def role_rows(role, sites, read, marks=None):
    """-> (rows, facts, problems) for one role"""
    problems, facts, rows = [], [], []
    surf, dest = [], []
    for kind, lst, out in (('surface', role['surface'], surf), ('texture', role['dest'], dest)):
        for s in lst:
            st = sites.get(s)
            if st is None or st['creator'] != kind:
                problems.append(f"{role['role']}: site 0x{s:08X} is not a {kind} create in the index")
                continue
            out.append((s, st, decode(st['value']) if st['value'] is not None else None))
    if PLANT == 'no_dest':
        dest = []
    oimg, quant = [], []
    for f, fn, var in role['ours']:
        got, p = ours_formats(read, f, fn, var)
        if p:
            problems.append(f"{role['role']}: {p}")
        oimg += [dict(g, fn=fn, var=var) for g in got]
    if role['fx']:
        quant, qp = ours_quant(read, role)
        problems += [f"{role['role']}: {x}" for x in qp]
    for field in FIELDS:
        src = (dest or surf) if field == 'precision' else surf
        gvals, gunk, game = [], [], []
        for s, st, d in src:
            v = g_val(field, d, dest) if d is not None and d['name'] is not None else None
            if d is None:
                gunk.append(f"0x{s:08X} {st['who']}: {st['reg']} = {st['text']} (not a constant)")
            elif d['name'] is None:
                gunk.append(f"0x{s:08X}: word {d['word']} format {d['fmt']} not decoded here")
            elif v is None:
                gunk.append(f"0x{s:08X}: {d['name']} has no colour-target form")
            else:
                gvals.append(v)
            game.append({'site': f'0x{s:08X}', 'fn': st['fn'], 'who': st['who'], 'creator': st['creator'], 'word': d['word'] if d else None,
                         'size': st['size'], 'value': v})
            facts.append({'side': 'game', 'fn': st['fn'], 'field': field, 'value': v if v is not None else {'reg': st['text']},
                          'cond': None, 'at': f'0x{s:08X}', 'role': role['role']})
        ovals, ounk, ours = [], [], []
        for g in oimg:
            if g['format'] == 'Depth':
                continue
            if g['format'] is None:
                ounk.append(f"{g['at']}: {g['var']}.format = {g['expr']} (not a literal)")
                v = None
            else:
                v = o_val(field, g['format'], quant if field == 'precision' else None)
                if v is None:
                    ounk.append(f"{g['at']}: rdr::Format::{g['format']} not in RFMT")
                else:
                    ovals.append(v)
            ours.append({'at': g['at'], 'fn': g['fn'], 'value': v})
            facts.append({'side': 'ours', 'fn': g['fn'], 'field': field, 'value': v if v is not None else {'expr': g['expr']},
                          'cond': None, 'at': g['at'], 'role': role['role']})
        verdict, detail = judge(field, gvals, ovals, gunk, ounk, marks, [g['at'] for g in oimg])
        rows.append({'role': role['role'], 'field': field, 'verdict': verdict, 'detail': detail, 'pair': role['pair'], 'why': role['why'],
                     'game': game, 'ours': ours, 'quant': quant if field == 'precision' else [],
                     'game_values': sorted({show(field, v) for v in gvals}), 'ours_values': sorted({show(field, v) for v in ovals})})
    return rows, facts, problems


def build(sites, read, roles=None, marks=None):
    rows, facts, problems = [], [], []
    for r in roles or ROLES:
        a, b, c = role_rows(r, sites, read, marks)
        rows += a
        facts += b
        problems += c
    named = {s for r in roles or ROLES for s in r['surface'] + r['dest']}
    unpaired = [dict(st, site=f'0x{s:08X}', decoded=(decode(st['value']) if st['value'] is not None else None))
                for s, st in sorted(sites.items()) if s not in named]
    return rows, facts, problems, unpaired


def run(con, a, read=None, sites=None):
    read = read or O.default_reader(a.ref)
    sites = sites if sites is not None else default_sites(con)
    try:
        import qcodediff
        marks = qcodediff.Marks(read)
    except ImportError:
        marks = None
    rows, facts, problems, unpaired = build(sites, read, marks=marks)
    sel = [r for r in rows if (not a.role or fnmatch.fnmatchcase(r['role'], a.role)) and (not a.field or r['field'] == a.field)
           and (not a.flag or r['verdict'] in a.flag)]
    if a.facts:
        keep = {(r['role'], r['field']) for r in sel}
        for f in facts:
            if (f['role'], f['field']) in keep:
                print(json.dumps(f, sort_keys=True))
    elif a.json:
        print(json.dumps({'rows': sel, 'unpaired': unpaired, 'problems': problems}, indent=1, default=str))
    else:
        cur = None
        for r in sel:
            if r['role'] != cur:
                cur = r['role']
                print(f"== {cur}  (pair [{r['pair']}]: {r['why']})")
                shown = set()
                for g in [g for x in sel if x['role'] == cur for g in x['game']]:
                    if g['site'] not in shown:
                        shown.add(g['site'])
                        print(f"   game {g['creator']:7s} {g['who']:<18s} {g['site']}  {g['word'] or '(no constant)'}  {g['size']}")
                seen = set()
                for o in r['ours']:
                    if o['at'] not in seen:
                        seen.add(o['at'])
                        print(f"   ours {o['at']}  {o['fn']}")
            print(f"  {r['verdict']:9s} {r['field']:13s} game {' | '.join(r['game_values']) or '-'}   ours {' | '.join(r['ours_values']) or '-'}"
                  + (f"   -- {r['detail']}" if r['detail'] else ''))
            for q in r['quant']:
                print(f"            ours quantise: {q['variant']} ({q['steps']} steps) looked up at {q['at']}, shader {q['shader']}")
        tot = {v: sum(1 for r in sel if r['verdict'] == v) for v in VERDICTS}
        print(f"totals: {len(sel)} rows (of {len(rows)}), " + ', '.join(f'{v} {n}' for v, n in tot.items()))
        ranked = sorted((r for r in sel if r['verdict'] in ('DIFF', 'GAME-ONLY', 'UNSETTLED')), key=lambda r: (RANK[r['verdict']], r['role']))
        if ranked:
            print('ranked:')
            for r in ranked:
                print(f"  {r['verdict']:9s} {r['role']}.{r['field']}: game {' | '.join(r['game_values']) or '-'}; ours {' | '.join(r['ours_values']) or '-'}")
        if not a.role and not a.field and not a.flag:
            depth = [u for u in unpaired if u['decoded'] and u['decoded']['kind'] == 'depth']
            colour = [u for u in unpaired if u not in depth]
            print(f'create sites no role names (coverage, not judged): {len(colour)} colour / unknown, {len(depth)} depth (not listed)')
            for u in colour:
                d = u['decoded']
                print(f"  {u['creator']:7s} {u['who']:<18s} {u['site']}  {d['word'] + ' ' + str(d['name']) if d else u['text']}  {u['size']}")
    for p in problems:
        print('PROBLEM: ' + p)
    return 2 if problems else 0


def main(argv, con):
    ap = argparse.ArgumentParser(prog='q targetfmt', description=__doc__.split('\n\n')[0])
    ap.add_argument('--role', help='a role name or a glob (post_*)')
    ap.add_argument('--field', choices=FIELDS)
    ap.add_argument('--flag', nargs='+', choices=VERDICTS)
    ap.add_argument('--facts', action='store_true', help='the code-diff facts, one JSON object per line')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--ref', help='read our code at this git ref (git show) instead of the working tree')
    a = ap.parse_args(argv)
    try:
        return run(con, a)
    except TargetFmtError as e:
        print(f'q targetfmt: {e}', file=sys.stderr)
        return 2
