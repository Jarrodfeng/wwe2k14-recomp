"""S101 lane 9: the 2D pass's self test (the gate entry, tools/gate/lanes/pass2d.json).

    py -I tools/pass2d/selftest.py

Builds out/tools/pass2d_test.exe when it is missing or older than its sources, then:
  1. runs it (headless, validation layer on): three sizes (1280x720, 2560x1440, 3440x1440) must render with 0 validation errors;
  2. TEXT: for every text the pass's layout output (the JSON the exe writes) is compared with an INDEPENDENT reading of the files: the glyph
     boxes against the .yfb advance table (fontHD.pac parsed here, not by the C++), the string against string.pac (parsed here);
  3. ANCHORS: at every size (21:9 included) the right-anchored sprite's right edge is the screen's right edge, the centred sprite is centred, the
     corner sprites sit in their corners, text anchors land where the anchor rule says;
  4. planted faults, each must fail LOUDLY: a string id that does not exist and a texture name that does not exist (exit 3, named in stderr), a
     string with glyphs the font lacks (exit 5, the code point named: never a silent blank), and corrupted copies of the layout JSON (every check
     above must catch a 0.5 px glyph shift / a 5 px anchor shift: a check that cannot fail is no check);
  5. the contact sheet exists (out/s101/pass2d/sheet.png, 1280 wide).
No game run: the exe is a headless renderer test that only reads game_data/extracted of the main checkout. Pictures stay under out/ (CLAUDE.md rule 1).
"""
import copy
import json
import os
import struct
import subprocess
import sys
import time
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
_cands = [Path(os.environ[k]) for k in ('RDR_MAIN_CHECKOUT',) if k in os.environ] + [ROOT, ROOT.parents[2]]
MAIN = next((c for c in _cands if (c / 'game_data' / 'extracted' / 'pac').is_dir()), _cands[0])
GAME = MAIN / 'game_data' / 'extracted'
OUT = MAIN / 'out' / 's101' / 'pass2d'
EXE = ROOT / 'out' / 'tools' / 'pass2d_test.exe'
EXE_GAPS = ROOT / 'out' / 'tools' / 'pass2d_gaps_test.exe'  # S101 lane 23
SOURCES = [ROOT / 'tools' / 'pass2d' / n for n in ('pass2d_test.cpp', 'gaps_test.cpp', 'build.ps1')] + \
          [ROOT / 'src' / 'renderer_scene' / n for n in ('pass2d.cpp', 'pass2d.h', 'ui_assets.cpp', 'ui_assets.h', 'scene_record.h')] + \
          [ROOT / 'src' / 'renderer' / 'shaders' / n for n in ('rdr_2d_quad.vert', 'rdr_2d_quad.frag', 'rdr_2d_yuv.frag')]
SIZES = [(1280, 720), (2560, 1440), (3440, 1440)]
EPS = 0.02


# ---------------------------------------------------------------------------------------------------------------- independent readers
def pach(buf, base):
    n = struct.unpack_from('<I', buf, base + 4)[0]
    first = base + 8 + 12 * n
    out = {}
    for i in range(n):
        eid, off, size = struct.unpack_from('<III', buf, base + 8 + 12 * i)
        out[eid] = buf[first + off:first + off + size]
    return out


def unzlib(leaf):
    assert leaf[:4] == b'ZLIB', 'not a ZLIB block'
    packed = struct.unpack_from('<I', leaf, 8)[0]
    return zlib.decompress(leaf[16:16 + packed])


class Font(dict):
    """{codepoint: (x, y, w, h)}; .kerns = {(left, right): value} from the .yfb pair list (S101 U1)."""
    kerns = {}
    index = 0


# The pen rule (S101 U1, docs/renderer/UI_PROMOTION_MAP.md section 6, measured on the CAW zip; written here from the stated rule, not from the C++):
# the next glyph's left = this glyph's left + (w - 2 - kern(this, next)) x scale; kern 0 for the last glyph of a line.
TRIM = 2  # font000 / font001; font002 is 7 (lane 62's census: 41,792 / 41,792 editor pairs, 1,907 / 1,907 menu pairs)
TRIMS = {0: 2, 1: 2, 2: 7}


def advances(text, font, trim=None, kern=True):
    """Per-character advances in sheet pixels for one logical line (the pass draws only the codes of this text that the font has)."""
    trim = TRIMS[font.index] if trim is None else trim
    out = []
    for i, c in enumerate(text):
        k = font.kerns.get((ord(c), ord(text[i + 1])), 0) if kern and i + 1 < len(text) else 0
        out.append(font[ord(c)][2] - trim - k)
    return out


def read_font(index=0):
    """font000.yfb: {codepoint: (x, y, w, h)} from pac/menu/fontHD.pac, entry 1 (docs/formats/ui_font.md)."""
    data = (GAME / 'pac' / 'menu' / 'fontHD.pac').read_bytes()
    table = unzlib(pach(data, 0x4000)[10 * index + 1])
    assert table[0x10:0x20] == f'name=font00{index}.yfb'.encode(), f'font table not font00{index}.yfb'
    n = struct.unpack_from('<I', table, 0xA4)[0]
    codes = 0xE8
    recs = (codes + 2 * n + 7) & ~7
    glyphs = Font()
    glyphs.index = index
    for k in range(n):
        c = struct.unpack_from('<H', table, codes + 2 * k)[0]
        glyphs[c] = struct.unpack_from('<4H', table, recs + 16 * k)
    # the kerning pair list: after the glyph records, `count` (u32 at +0xB4) records of u16 left, u16 right, u32 value
    tail = recs + 16 * n
    count = struct.unpack_from('<I', table, 0xB4)[0]
    glyphs.kerns = {}
    for k in range(count):
        l, r, v = struct.unpack_from('<HHI', table, tail + 8 * k)
        glyphs.kerns[(l, r)] = v
    assert glyphs.kerns and max(glyphs.kerns.values()) <= 8, 'the kerning pair list did not read as pairs'
    return glyphs


def read_strings():
    """{id: text} from pac/string.pac SDB entries 0000 then 5000 (first wins), independent of the C++ reader."""
    d = (GAME / 'pac' / 'string.pac').read_bytes()
    dir_bytes = struct.unpack_from('<I', d, 4)[0]
    p, ents = 0x800, {}
    while p + 12 <= 0x800 + dir_bytes:
        words, kind = struct.unpack_from('<HH', d, p + 4)
        p += 12
        rs = 16 if kind == 1 else 12
        for _ in range(words * 4 // rs):
            name = d[p:p + rs - 8].rstrip(b' \0').decode('latin1')
            sec, sz = struct.unpack_from('<II', d, p + rs - 8)
            p += rs
            ents.setdefault(name, d[0x4000 + sec * 0x800:0x4000 + sec * 0x800 + sz * 256])
    strings = {}
    for want in ('0000', '5000'):
        for leaf in pach(ents[want], 0).values():
            try:
                t = unzlib(leaf)
            except Exception:
                continue  # (an empty table leaf)
            if len(t) < 8:
                continue
            for i in range(struct.unpack_from('<I', t, 4)[0]):
                off, ln, sid = struct.unpack_from('<III', t, 8 + 12 * i)
                strings.setdefault(sid, t[off:off + ln - 1].decode('utf8'))
    return strings


# ---------------------------------------------------------------------------------------------------------------- the checks
# The test scene's texts by item index: (string id, glyph cell height in layout units, align, anchor, x in layout units, y). Anchor: L left edge,
# C centred box, R right edge (horizontal only: that is what the layout rule changes with the screen's shape).
TEXTS = [
    (282, 48, 1, 'C', 640), (303, 28, 0, 'L', 60), (304, 28, 2, 'R', 1220), (60100, 20, 0, 'L', 60), (60101, 20, 0, 'L', 200),
    (228, 20, 0, 'L', 60), (228, 40, 0, 'L', 60), (263, 28, 1, 'C', 480), (264, 28, 1, 'C', 640), (265, 28, 1, 'C', 800),
    (274, 24, 2, 'R', 1220), (284, 36, 1, 'C', 640),
]


def origin_x(kind, width, s):
    return {'L': 0.0, 'C': (width - 1280 * s) / 2, 'R': width - 1280 * s}[kind]


def check_layout(lay, font, strings, rule={}):
    """Every violation as a string; empty = the layout output agrees with the files and the anchor rules."""
    bad = []
    w, h = lay['width'], lay['height']
    s = h / 720.0
    if abs(lay['scale'] - s) > 1e-5:
        bad.append(f'{w}x{h}: scale {lay["scale"]} != {s}')
    if lay['missing_glyphs']:
        bad.append(f'{w}x{h}: glyphs reported missing: {lay["missing_glyphs"]}')
    cell = max(g[3] for g in font.values())
    texts = lay['texts']
    if len(texts) != len(TEXTS):
        bad.append(f'{w}x{h}: {len(texts)} texts laid out, expected {len(TEXTS)}')
        return bad
    for t, (sid, size, align, anchor, x) in zip(texts, TEXTS):
        tag = f'{w}x{h} text {t["item"]} (id {sid})'
        if t['string_id'] != sid:
            bad.append(f'{tag}: string id {t["string_id"]}')
        if sid not in strings:
            bad.append(f'{tag}: id not in string.pac read independently')
            continue
        if t['utf8'] != strings[sid]:
            bad.append(f'{tag}: string differs from string.pac')
        if abs(t['size_px'] - size * s) > EPS or abs(t['scale'] - size * s / cell) > 1e-6:
            bad.append(f'{tag}: size {t["size_px"]} / scale {t["scale"]}, expected {size * s} / {size * s / cell}')
        if t['align'] != align:
            bad.append(f'{tag}: align {t["align"]}')
        ax = origin_x(anchor, w, s) + x * s
        if abs(t['anchor_x'] - ax) > EPS:
            bad.append(f'{tag}: anchor x {t["anchor_x"]}, rule says {ax}')
        scale = size * s / cell
        lines = strings[sid].split('\n')
        want = []
        for li, line in enumerate(lines):
            adv = [a * scale for a in advances(line, font, **rule)]
            width = sum(adv)
            x0 = ax - (width / 2 if align == 1 else width if align == 2 else 0)
            for c, a in zip(line, adv):
                want.append((ord(c), li, x0, a))
                x0 += a
        got = t['glyphs']
        if len(got) != len(want):
            bad.append(f'{tag}: {len(got)} glyph boxes, the string has {len(want)}')
            continue
        for g, (cp, li, wx, wa) in zip(got, want):
            if g[0] != cp or g[1] != li:
                bad.append(f'{tag}: glyph order {g[:2]} != {(cp, li)}')
                break
            if abs(g[2] - wx) > EPS or abs(g[4] - font[cp][2] * scale) > EPS:
                bad.append(f'{tag}: glyph U+{cp:04X} at x {g[2]:.3f} w {g[4]:.3f}, the pen rule says x {wx:.3f}, the quad is w {font[cp][2] * scale:.3f}')
                break
            if abs(g[5] - font[cp][3] * scale) > EPS or abs(g[3] - (t['anchor_y'] + li * size * s)) > EPS:
                bad.append(f'{tag}: glyph U+{cp:04X} y / height off')
                break
    sp = {x['widget']: x['rect'] for x in lay['sprites']}
    for name in ('background', 'right_edge', 'centre', 'top_left', 'bottom_right', 'icon0'):
        if name not in sp:
            bad.append(f'{w}x{h}: sprite {name} missing from the layout')
    if bad:
        return bad
    x, y, rw, rh = sp['right_edge']
    if abs(x + rw - w) > EPS:
        bad.append(f'{w}x{h}: right-anchored sprite ends at {x + rw}, the screen edge is {w}')
    x, y, rw, rh = sp['centre']
    if abs(x + rw / 2 - w / 2) > EPS:
        bad.append(f'{w}x{h}: centred sprite centre {x + rw / 2}, screen centre {w / 2}')
    x, y, rw, rh = sp['top_left']
    if abs(x) > EPS or abs(y) > EPS:
        bad.append(f'{w}x{h}: top-left sprite at {x},{y}')
    x, y, rw, rh = sp['bottom_right']
    if abs(x + rw - w) > EPS or abs(y + rh - h) > EPS:
        bad.append(f'{w}x{h}: bottom-right sprite ends at {x + rw},{y + rh}')
    if [round(v, 3) for v in sp['background']] != [0, 0, w, h]:
        bad.append(f'{w}x{h}: the stretched background is {sp["background"]}')
    if abs(sp['icon0'][0] - 60 * s) > EPS:
        bad.append(f'{w}x{h}: left-anchored icon at {sp["icon0"][0]}, expected {60 * s}')
    return bad


def run(args, out_dir, timeout=240, exe=None):
    # (another lane's program can hold the GPU's host-visible memory for a moment: "Not enough graphics memory for the frame ring" is retried, any other failure is not)
    for attempt in range(24):
        p = subprocess.run([str(exe or EXE), '--game', str(GAME), '--out', str(out_dir)] + args, capture_output=True, text=True, timeout=timeout)
        if p.returncode == 0 or 'frame ring' not in (p.stdout + p.stderr):
            break
        time.sleep(5)
    return p.returncode, p.stdout, p.stderr


def main():
    if not GAME.is_dir():
        print(f'FAIL: game files not found at {GAME}')
        return 1
    stale = not EXE.is_file() or not EXE_GAPS.is_file() or any(src.is_file() and src.stat().st_mtime > min(EXE.stat().st_mtime, EXE_GAPS.stat().st_mtime) for src in SOURCES)
    if stale:
        b = subprocess.run(['pwsh', '-NoProfile', '-File', str(ROOT / 'tools' / 'pass2d' / 'build.ps1'), '-NoShaders'], capture_output=True, text=True, timeout=900)
        if b.returncode != 0 or not EXE.is_file() or not EXE_GAPS.is_file():
            print('FAIL: build failed: ' + ' | '.join((b.stdout + b.stderr).strip().splitlines()[-3:])[:400])
            return 1
    font, strings = read_font(), read_strings()

    # 1. the three sizes, validation on
    rc, out, err = run([], OUT)
    if rc != 0:
        print(f'FAIL: render exit {rc}: ' + (out + err).strip().replace('\n', ' | ')[-400:])
        return 1
    if out.count('validation errors so far 0') != 3 or 'validation errors 0' not in out:
        print('FAIL: validation errors reported: ' + out.strip().replace('\n', ' | ')[-300:])
        return 1
    # 2 + 3. text and anchors, from the layout output
    lays = {}
    for w, h in SIZES:
        f = OUT / f'pass2d_{w}x{h}.json'
        png = OUT / f'pass2d_{w}x{h}.png'
        if not f.is_file() or not png.is_file() or png.stat().st_size < 10000:
            print(f'FAIL: {f.name} / {png.name} missing or empty')
            return 1
        lays[(w, h)] = json.loads(f.read_text(encoding='utf-8'))
        bad = check_layout(lays[(w, h)], font, strings)
        if bad:
            print('FAIL: ' + ' | '.join(bad[:3]))
            return 1
    glyph_count = sum(len(t['glyphs']) for t in lays[SIZES[0]]['texts'])
    # 4a. planted fault twins on the layout checks
    for label, mutate in (
        ('glyph shifted 0.5 px', lambda L: L['texts'][0]['glyphs'][3].__setitem__(2, L['texts'][0]['glyphs'][3][2] + 0.5)),
        ('glyph advance changed', lambda L: L['texts'][6]['glyphs'][2].__setitem__(4, L['texts'][6]['glyphs'][2][4] * 1.05)),
        ('right-anchored sprite moved 5 px', lambda L: [x['rect'].__setitem__(0, x['rect'][0] - 5) for x in L['sprites'] if x['widget'] == 'right_edge']),
        ('centred sprite moved 5 px', lambda L: [x['rect'].__setitem__(0, x['rect'][0] + 5) for x in L['sprites'] if x['widget'] == 'centre']),
        ('text anchor moved', lambda L: L['texts'][2].__setitem__('anchor_x', L['texts'][2]['anchor_x'] - 5)),
        ('a glyph reported missing', lambda L: L['missing_glyphs'].append(57356)),
    ):
        broken = copy.deepcopy(lays[SIZES[2]])
        mutate(broken)
        if not check_layout(broken, font, strings):
            print(f'FAIL: planted fault not caught: {label}')
            return 1
    # 4a'. S101 U1: the OLD advance (advance = w, no trim, no kerning) re-found as a plant, two ways: the checker holding the old rule must fail on the
    # real layout, and the pass itself run with the old rule (--fault advance_w: a font with trim 0 and no pair list) must fail the checker holding the new one
    if not check_layout(lays[SIZES[2]], font, strings, rule={'trim': 0, 'kern': False}):
        print('FAIL: the old advance rule (advance = w) as a checker is not caught by the layout it should disagree with')
        return 1
    rc, out, err = run(['--fault', 'advance_w'], OUT / 'fault_advance_w')
    old_lay = json.loads((OUT / 'fault_advance_w' / f'pass2d_{SIZES[2][0]}x{SIZES[2][1]}.json').read_text(encoding='utf-8')) if rc == 0 else None
    if old_lay is None or not check_layout(old_lay, font, strings) or check_layout(old_lay, font, strings, rule={'trim': 0, 'kern': False}):
        print(f'FAIL: planted fault advance_w (the pass with the old pen rule) was not caught by the measured rule, or not recognised by the old one (render exit {rc})')
        return 1
    # 4a''. S101 U1 (lane 62's census): the trim is per font, c = 7 for font002. The page drawn with font002 must match the c = 7 rule, and the pass run with
    # c = 2 (--fault advance_c2: the other fonts' constant) must fail it. (Strings the font lacks make the render report them: then the check is skipped, said.)
    f2 = read_font(2)
    rc, out, err = run(['--font', '2'], OUT / 'font2')
    if rc != 0 or not (OUT / 'font2' / f'pass2d_{SIZES[2][0]}x{SIZES[2][1]}.json').is_file():
        print(f'FAIL: the page drawn with font002 did not render (exit {rc}): ' + (out + err).strip().replace('\n', ' | ')[-300:])
        return 1
    lay2 = json.loads((OUT / 'font2' / f'pass2d_{SIZES[2][0]}x{SIZES[2][1]}.json').read_text(encoding='utf-8'))
    bad2 = check_layout(lay2, f2, strings)
    if bad2:
        print('FAIL: font002 layout disagrees with the c = 7 pen rule: ' + ' | '.join(bad2[:2]))
        return 1
    rc, out, err = run(['--font', '2', '--fault', 'advance_c2'], OUT / 'font2_c2')
    c2 = json.loads((OUT / 'font2_c2' / f'pass2d_{SIZES[2][0]}x{SIZES[2][1]}.json').read_text(encoding='utf-8')) if rc == 0 else None
    if c2 is None or not check_layout(c2, f2, strings):
        print(f'FAIL: planted fault advance_c2 (font002 with c = 2) was not caught by the per-font rule (render exit {rc})')
        return 1
    # 4b. planted faults in the files: each must fail loudly, with its own exit code and the thing named
    for fault, code, needle in (('string', 3, '999999'), ('texture', 3, 'no_such_texture'), ('glyph', 5, 'U+E00C')):
        rc, out, err = run(['--fault', fault], OUT / ('fault_' + fault))
        if rc != code or needle not in (out + err):
            print(f'FAIL: --fault {fault}: exit {rc} (want {code}), message lacks {needle!r}: ' + (out + err).strip().replace('\n', ' | ')[-300:])
            return 1
    # 4c. a missing game folder fails loudly too
    p = subprocess.run([str(EXE), '--game', str(OUT / 'no_such_game_dir'), '--out', str(OUT / 'fault_nogame')], capture_output=True, text=True, timeout=120)
    if p.returncode == 0:
        print('FAIL: a missing game folder did not fail')
        return 1
    # 6. S101 lane 23: the gap scenarios (a4 / blend / gpu / movie / text), each at three sizes, checked by gaps_check.py against independent readings
    gaps_msg, gaps_rc = run_gaps(font)
    if gaps_rc:
        return 1
    # 5. the sheet (the gap program rewrites it with the labelled panels: the three menu-page pictures above and the gap scenarios)
    sheet = OUT / 'sheet.png'
    if not sheet.is_file() or struct.unpack('>II', sheet.read_bytes()[16:24])[0] != 1280 or struct.unpack('>II', sheet.read_bytes()[16:24])[1] < 1500:
        print('FAIL: sheet.png missing, not 1280 wide, or without the gap panels')
        return 1
    print(f'PASS pass2d selftest: 3 sizes 0 validation errors; {len(TEXTS)} texts / {glyph_count} glyphs per size match the .yfb advances and string.pac; anchors hold at 16:9, 2x and 21:9; '
          f'6 planted layout faults + the old advance (advance = w) caught; missing string id / texture name / glyph (U+E00C) fail loudly; {gaps_msg}; sheet {sheet}')
    return 0


GAPS_OUT = OUT / 'gaps'
A4_PAC, A4_TOP, A4_LEAF, A4_NAME = 'pac/cab/CAB.pac', 144, 0, 'Mask1'  # the smallest A4R4G4B4 texture of the atlas (256 x 256, model directory p144/0)


def run_gaps(font):
    """The five gap scenarios and their planted faults. Returns (summary, 0) or (message already printed, 1)."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import gaps_check as gc
    try:
        truth = gc.a4_ground_truth(GAME, A4_PAC, A4_TOP, A4_LEAF, A4_NAME)
    except Exception as e:  # noqa: BLE001 - the independent reader failing is a failure of the check, said loudly
        print(f'FAIL: the independent A4R4G4B4 reader could not read {A4_PAC} p{A4_TOP}/{A4_LEAF}:{A4_NAME}: {e}')
        return '', 1
    spec = A4_PAC.replace('/', '\\') + f'|p{A4_TOP}/{A4_LEAF}:{A4_NAME}'
    rc, out, err = run(['--a4', spec, '--base', str(OUT)], GAPS_OUT, exe=EXE_GAPS, timeout=600)
    if rc != 0:
        print(f'FAIL: gaps render exit {rc}: ' + (out + err).strip().replace('\n', ' | ')[-500:])
        return '', 1
    if out.count('validation errors so far 0') != 15 or 'validation errors 0' not in out:
        print('FAIL: gaps: validation errors or a missing frame: ' + out.strip().replace('\n', ' | ')[-300:])
        return '', 1
    shutil_sheet = GAPS_OUT / 'sheet.png'
    if not shutil_sheet.is_file():
        print('FAIL: the gap program wrote no sheet')
        return '', 1
    (OUT / 'sheet.png').write_bytes(shutil_sheet.read_bytes())
    checks = {
        'a4': lambda d: gc.check_a4(d, truth), 'blend': gc.check_blend, 'gpu': gc.check_gpu, 'movie': gc.check_movie, 'text': lambda d: gc.check_text(d, font),
    }
    for name, check in checks.items():
        bad = check(GAPS_OUT)
        if bad:
            print(f'FAIL: gaps {name}: ' + ' | '.join(bad[:3]))
            return '', 1
    # planted faults: each is a defect of one item's class and its check must catch it
    #   a4_raw       item 1 (A4R4G4B4 words uploaded as the file holds them: the channel order)
    #   gpu_as_file  item 2 (the select-screen garbage / title logo class, S100 ledger rows 14 / 20-22: a sprite that shows a GPU picture draws a file texture)
    #   yuv_swap     item 3, blend_wrong item 4, wrap_off / clip_off item 5
    for fault, scenario in (('a4_raw', 'a4'), ('gpu_as_file', 'gpu'), ('yuv_swap', 'movie'), ('pad_ignored', 'movie'), ('blend_wrong', 'blend'), ('wrap_off', 'text'), ('clip_off', 'text'), ('advance_w', 'text')):
        d = OUT / ('fault_gaps_' + fault)
        rc, out, err = run(['--a4', spec, '--only', scenario, '--fault', fault], d, exe=EXE_GAPS, timeout=300)
        if rc != 0:
            print(f'FAIL: gaps --fault {fault}: the render itself failed, exit {rc}: ' + (out + err).strip().replace('\n', ' | ')[-300:])
            return '', 1
        if not checks[scenario](d):
            print(f'FAIL: gaps planted fault not caught: {fault} ({scenario})')
            return '', 1
    # loud faults: the pass says what it could not draw
    for fault, scenario, needle in (('bad_role', 'gpu', "'present'"), ('no_planes', 'movie', 'planes'), ('self_sample', 'gpu', 'draws into'), ('extra_short', 'text', 'text_extra'), ('clip_nan', 'text', 'clip')):
        rc, out, err = run(['--a4', spec, '--only', scenario, '--fault', fault], OUT / ('fault_gaps_' + fault), exe=EXE_GAPS, timeout=300)
        if rc != 5 or needle not in (out + err):
            print(f'FAIL: gaps --fault {fault}: exit {rc} (want 5), message lacks {needle!r}: ' + (out + err).strip().replace('\n', ' | ')[-300:])
            return '', 1
    return ('gaps: A4R4G4B4 texels, 6 blend formulas, a GPU picture by role, 2 YUV movies (backdrop fit + alpha), wrapped / clipped text all match their independent readings at 3 sizes; '
            '6 planted faults caught (a4 channel order, GPU picture drawn as a file texture, swapped chroma, additive as straight, wrap off, clip off); '
            'a Present-role sprite target and missing movie planes fail loudly'), 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except subprocess.TimeoutExpired as e:  # a hung exe / build is a FAIL line, not a traceback
        print(f'FAIL: timeout after {e.timeout:.0f} s: {" ".join(str(a) for a in e.cmd)[:200]}')
        sys.exit(1)
