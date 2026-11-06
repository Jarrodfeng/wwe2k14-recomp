"""Session 93 (ASTC / EAC transcode, idea 2): offline bit test of the BC -> ASTC / EAC load shaders. No game launch.

    py -I tools\\native\\compute_test\\transcode_test.py [--blocks N] [--seed S] [--only addr|ref|bc1|bc4|bc5]

What it does (all on the PC's GPU through run_compute.exe, nothing from the game is read):
  addr  the untiling / endian arithmetic of the new shaders (COPY64 / COPY128 builds of
        src/graphics/shaders/vulkan_glsl/texture_transcode_cs.glsl) against the shipped texture_load_64bpb / 128bpb shaders
        (SPIR-V headers in the SDK worktree): linear, tiled 2D and tiled 3D, endian 0..3, several sizes. Must be identical.
  ref   our CPU BC1 / BC4 / BC5 decoders against the shipped DXT1 -> RGBA8, DXT5A -> R8, DXN -> RG8 shaders (so that the
        reference the transcode is measured against is the decode the game uses today). Tolerance 1 (rounding of 1/3).
  bc1   BC1 -> ASTC 4x4: GPU output decoded by a CPU ASTC decoder written for exactly the subset emitted (block modes 0x042
        and 0x451, CEM 8 / 12) vs the CPU BC1 decode. bc4 / bc5: BC4 / BC5 -> EAC R11 / RG11, decoded by a CPU EAC decoder.
Decoders written from the Khronos Data Format Specification (EAC: etc2 chapter "Format Unsigned R11 EAC"; ASTC chapter:
block mode, color endpoint mode, integer sequence encoding, endpoint / weight unquantization, dual-plane decoding,
data size determination). Errors are in 8-bit units (decoded value * 255 for unorm), against the exact (unrounded) BC decode.
The corpus is synthetic (random + edge cases) and generated at run time; the work files are under out\\native\\compute_test\\."""
import argparse
import math
import re
import subprocess
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[3]
EXE = ROOT / 'out' / 'native' / 'compute_test' / 'run_compute.exe'
WORK = ROOT / 'out' / 'native' / 'compute_test' / 'transcode'
import os
SDK = Path(os.environ['TRANSCODE_SDK']) if os.environ.get('TRANSCODE_SDK') else ROOT / 'thirdparty' / 'sdk-wt' / 's94-tex2'
GLSL = SDK / 'src' / 'graphics' / 'shaders' / 'vulkan_glsl' / 'texture_transcode_cs.glsl'
SPV_DIR = SDK / 'src' / 'graphics' / 'shaders' / 'vulkan_spirv'
GLSLANG = ROOT / 'out' / 'glslang-build' / 'StandAlone' / 'glslangValidator.exe'


# ----------------------------------------------------------------------------------------------------------------------
# running
# ----------------------------------------------------------------------------------------------------------------------
# Session 94: 1 LSB is not reachable: EAC has 16 fixed 8-level tables, BC4's 8 levels are evenly spaced; the exhaustive minimax search over all
# 16 x 16 x 256 (table, multiplier, base) reaches max ~12.2 on the worst random blocks of this corpus (smooth real alpha is far lower).
EAC_BAR_MAX = 14.0
EAC_BAR_MEAN = 1.35

def build_variant(define):
    WORK.mkdir(parents=True, exist_ok=True)
    spv = WORK / (define.replace(',', '_') + '.spv')
    r = subprocess.run([str(GLSLANG), '-V', '--target-env', 'vulkan1.0'] + [f'-D{d}' for d in define.split(',')] + ['-DTEST_ONE_SET', '-S', 'comp',
                        '-o', str(spv), str(GLSL)], capture_output=True, text=True)
    if r.returncode:
        sys.exit('glslang failed:\n' + r.stdout + r.stderr)
    return spv


def shipped_spv(name):
    text = (SPV_DIR / f'{name}.h').read_text(encoding='utf-8')
    body = text.split('[] = {', 1)[1].split('};', 1)[0]
    words = [int(w, 16) for w in re.findall(r'0x[0-9A-Fa-f]{8}', body)]
    spv = WORK / f'shipped_{name}.spv'
    spv.write_bytes(np.array(words, dtype='<u4').tobytes())
    return spv


def push_bytes(flags, goff, gpitch, gz, size, hoff, hpitch, htex):
    return np.array([flags, goff, gpitch, gz, size[0], size[1], size[2], hoff, hpitch, htex], dtype='<u4').tobytes()


def run_gpu(spv, groups, push, src, dst_bytes, two_sets):
    """Runs one dispatch; returns (destination bytes, microseconds). two_sets: shipped layout (dst set 0, src set 1)."""
    WORK.mkdir(parents=True, exist_ok=True)
    (WORK / 'push.bin').write_bytes(push)
    (WORK / 'src.bin').write_bytes(src)
    dst_b, src_b = ('0/0', '1/0') if two_sets else ('0/0', '0/1')
    cmd = [str(EXE), '--spv', str(spv), '--groups', *[str(g) for g in groups], '--push', str(WORK / 'push.bin'),
           '--bind', f'{dst_b}:ssbo:r8:{dst_bytes}:1:-', '--bind', f'{src_b}:ssbo:r8:{len(src)}:1:{WORK / "src.bin"}',
           '--out', f'{dst_b}:{WORK / "dst.bin"}']
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit('run_compute failed:\n' + r.stdout + r.stderr)
    m = re.search(r'dispatch .*: ([0-9.]+) us', r.stdout)
    gpu = re.search(r'GPU: (.*)', r.stdout).group(1)
    run_gpu.gpu = gpu
    return (WORK / 'dst.bin').read_bytes(), float(m.group(1))


run_gpu.gpu = '?'


# ----------------------------------------------------------------------------------------------------------------------
# CPU decoders
# ----------------------------------------------------------------------------------------------------------------------
def exp565(c):
    r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
    return np.array([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], dtype=np.float64)


def bc1_decode(w0, w1):
    """-> (16,4) float RGBA in 0..255, texel index y*4+x. Exact thirds (not rounded)."""
    c0, c1 = w0 & 0xFFFF, w0 >> 16
    e0, e1 = exp565(c0), exp565(c1)
    pal = np.zeros((4, 4))
    pal[0, :3], pal[1, :3] = e0, e1
    pal[:, 3] = 255
    if c0 > c1:
        pal[2, :3] = (2 * e0 + e1) / 3
        pal[3, :3] = (e0 + 2 * e1) / 3
    else:
        pal[2, :3] = (e0 + e1) / 2
        pal[3, :] = 0
    return np.array([pal[(w1 >> (2 * t)) & 3] for t in range(16)])


def bc4_levels(r0, r1):
    lv = [float(r0), float(r1)]
    if r0 > r1:
        lv += [((8 - k) * r0 + (k - 1) * r1) / 7.0 for k in range(2, 8)]
    else:
        lv += [((6 - k) * r0 + (k - 1) * r1) / 5.0 for k in range(2, 6)] + [0.0, 255.0]
    return lv


def bc4_decode(w0, w1):
    lv = bc4_levels(w0 & 255, (w0 >> 8) & 255)
    bits = (w0 >> 16) | (w1 << 16)  # 48 index bits
    return np.array([lv[(bits >> (3 * t)) & 7] for t in range(16)])


EAC_MOD = [[-3, -6, -9, -15, 2, 5, 8, 14], [-3, -7, -10, -13, 2, 6, 9, 12], [-2, -5, -8, -13, 1, 4, 7, 12],
           [-2, -4, -6, -13, 1, 3, 5, 12], [-3, -6, -8, -12, 2, 5, 7, 11], [-3, -7, -9, -11, 2, 6, 8, 10],
           [-4, -7, -8, -11, 3, 6, 7, 10], [-3, -5, -8, -11, 2, 4, 7, 10], [-2, -6, -8, -10, 1, 5, 7, 9],
           [-2, -5, -8, -10, 1, 4, 7, 9], [-2, -4, -8, -10, 1, 3, 7, 9], [-2, -5, -7, -10, 1, 4, 6, 9],
           [-3, -4, -7, -10, 2, 3, 6, 9], [-1, -2, -3, -10, 0, 1, 2, 9], [-4, -6, -8, -9, 3, 5, 7, 8],
           [-3, -5, -7, -9, 2, 4, 6, 8]]


def eac_decode(b8):
    """Unsigned R11 EAC (spec etc2 chapter): 8 bytes q0..q7 = big-endian int64. -> (16,) in 8-bit units (D * 255 / 2047),
    texel index y*4+x (EAC pixels are numbered column by column)."""
    v = int.from_bytes(bytes(b8), 'big')
    base, mult, table = (v >> 56) & 255, (v >> 52) & 15, (v >> 48) & 15
    out = np.zeros(16)
    for k in range(16):  # pixel k = a..p, x = k // 4, y = k % 4
        mod = EAC_MOD[table][(v >> (45 - 3 * k)) & 7]
        d = base * 8 + 4 + (mod * mult * 8 if mult else mod)
        d = min(max(d, 0), 2047)
        out[(k % 4) * 4 + (k // 4)] = d * 255.0 / 2047.0
    return out


def trit_decode(T):
    """Spec 'Integer Sequence Encoding': 8 bits T -> (t0..t4)."""
    def bits(h, l):
        return (T >> l) & ((1 << (h - l + 1)) - 1)
    if bits(4, 2) == 7:
        C = (bits(7, 5) << 2) | bits(1, 0)
        t4 = t3 = 2
    else:
        C = bits(4, 0)
        if bits(6, 5) == 3:
            t4, t3 = 2, (T >> 7) & 1
        else:
            t4, t3 = (T >> 7) & 1, bits(6, 5)

    def c(h, l):
        return (C >> l) & ((1 << (h - l + 1)) - 1)
    if c(1, 0) == 3:
        t2, t1, t0 = 2, c(4, 4), (c(3, 3) << 1) | (c(2, 2) & (1 - c(3, 3)))
    elif c(3, 2) == 3:
        t2, t1, t0 = 2, 2, c(1, 0)
    else:
        t2, t1, t0 = c(4, 4), c(3, 2), (c(1, 1) << 1) | (c(0, 0) & (1 - c(1, 1)))
    return [t0, t1, t2, t3, t4]


# weight range table (spec 'ASTC weight range encodings'): rho -> (max, trits, quints, bits) for P = 0 / 1
WEIGHT_RANGES = {(0, 2): (1, 0, 0, 1), (0, 3): (2, 1, 0, 0), (0, 4): (3, 0, 0, 2), (0, 5): (4, 0, 1, 0), (0, 6): (5, 1, 0, 1),
                 (0, 7): (7, 0, 0, 3), (1, 2): (9, 0, 1, 1), (1, 3): (11, 1, 0, 2), (1, 4): (15, 0, 0, 4),
                 (1, 5): (19, 0, 1, 2), (1, 6): (23, 1, 0, 3), (1, 7): (31, 0, 0, 5)}
# endpoint value ranges, largest first: (max, bits, trit, quint)
CEM_RANGES = [(255, 8, 0, 0), (191, 6, 1, 0), (159, 5, 0, 1), (127, 7, 0, 0), (95, 5, 1, 0), (79, 4, 0, 1), (63, 6, 0, 0),
              (47, 4, 1, 0), (39, 3, 0, 1), (31, 5, 0, 0), (23, 3, 1, 0), (19, 2, 0, 1), (15, 4, 0, 0), (11, 2, 1, 0),
              (9, 1, 0, 1), (7, 3, 0, 0), (5, 1, 1, 0), (3, 2, 0, 0)]


class AstcUnsupported(Exception):
    pass


def astc_decode(b16):
    """ASTC 4x4 LDR decoder for the subset the transcoder emits: single partition, 4x4 weight grid (no infill), bit-only
    endpoint ranges, weight ranges with bits or one trit, CEM 8 / 12, single or dual plane.
    -> ((16,4) RGBA in 8-bit units (UNORM16 * 255 / 65535), blue_contract_used)."""
    blk = int.from_bytes(bytes(b16), 'little')

    def bits(h, l):
        return (blk >> l) & ((1 << (h - l + 1)) - 1)
    mode = bits(10, 0)
    if bits(8, 0) == 0b111111100 or bits(3, 0) == 0:
        raise AstcUnsupported('void extent / reserved')
    if bits(1, 0) == 0 or bits(3, 2) != 0:
        raise AstcUnsupported(f'block mode {mode:#x}: not the 4x4 row of the table we emit')
    W, H = ((mode >> 7) & 3) + 4, ((mode >> 5) & 3) + 2
    rho = (((mode >> 1) & 1) << 2) | ((mode & 1) << 1) | ((mode >> 4) & 1)
    P, DP = (mode >> 9) & 1, (mode >> 10) & 1
    if (W, H) != (4, 4):
        raise AstcUnsupported('grid not 4x4')
    wmax, wt, wq, wb = WEIGHT_RANGES[(P, rho)]
    if wq:
        raise AstcUnsupported('quint weights')
    nw = W * H * (2 if DP else 1)
    weight_bits = math.ceil(nw * 8 * wt / 5) + nw * wb
    if bits(12, 11) != 0:
        raise AstcUnsupported('more than one partition')
    cem = bits(16, 13)
    cfg = 17 + (2 if DP else 0)
    remaining = 128 - cfg - weight_bits
    nvals = ((cem >> 2) + 1) * 2
    chosen = None
    for rng in CEM_RANGES:
        mx, nb, tr, qu = rng
        need = math.ceil(nvals * 8 * tr / 5) + math.ceil(nvals * 7 * qu / 3) + nvals * nb
        if need <= remaining:
            chosen = rng
            break
    if chosen is None or chosen[2] or chosen[3]:
        raise AstcUnsupported(f'endpoint range {chosen} needs trits / quints')
    mx, nb, _, _ = chosen
    vals = []
    for i in range(nvals):
        v = bits(17 + nb * i + nb - 1, 17 + nb * i)
        x, w = v, nb
        while w < 8:
            x = (x << nb) | v
            w += nb
        vals.append(x >> (w - 8))
    # weights: the stream is the block bit-reversed from bit 127 down
    stream = [(blk >> (127 - k)) & 1 for k in range(weight_bits)]
    raw = []
    if wt:
        # 5 values per 8 bits (n = 0): value i of group g is trit i of T(bits 8g..8g+7)
        g = 0
        while len(raw) < nw:
            T = 0
            for j in range(8):
                kk = 8 * g + j
                if kk < weight_bits:
                    T |= stream[kk] << j
            raw += trit_decode(T)
            g += 1
        raw = raw[:nw]
    else:
        for i in range(nw):
            raw.append(sum(stream[i * wb + j] << j for j in range(wb)))
    # unquantize to 0..64 (spec 'Weight Unquantization')
    def unq(v):
        if wt:  # 0..2 -> 0, 32, 63
            u = [0, 32, 63][v]
        else:
            x, w = v, wb
            while w < 6:
                x = (x << wb) | v
                w += wb
            u = x >> (w - 6) if w > 6 else x
            if wb >= 5:
                raise AstcUnsupported('5-bit weights not expected')
        return u + 1 if u > 32 else u
    weights = [unq(v) for v in raw]
    ccs = bits(128 - weight_bits - 2 + 1, 128 - weight_bits - 2) if DP else None
    contract = False

    def bc(r, g, b, a):
        return ((r + b) >> 1, (g + b) >> 1, b, a)
    if cem == 8:
        v = vals
        if v[1] + v[3] + v[5] >= v[0] + v[2] + v[4]:
            e0, e1 = (v[0], v[2], v[4], 255), (v[1], v[3], v[5], 255)
        else:
            e0, e1, contract = bc(v[1], v[3], v[5], 255), bc(v[0], v[2], v[4], 255), True
    elif cem == 12:
        v = vals
        if v[1] + v[3] + v[5] >= v[0] + v[2] + v[4]:
            e0, e1 = (v[0], v[2], v[4], v[6]), (v[1], v[3], v[5], v[7])
        else:
            e0, e1, contract = bc(v[1], v[3], v[5], v[7]), bc(v[0], v[2], v[4], v[6]), True
    else:
        raise AstcUnsupported(f'CEM {cem}')
    out = np.zeros((16, 4))
    for t in range(16):
        for ch in range(4):
            w = weights[t * 2 + 1] if (DP and ch == ccs) else weights[t * (2 if DP else 1)]
            c0, c1 = (e0[ch] << 8) | e0[ch], (e1[ch] << 8) | e1[ch]
            c = (c0 * (64 - w) + c1 * w + 32) >> 6
            out[t, ch] = c * 255.0 / 65535.0
    return out, contract


# ----------------------------------------------------------------------------------------------------------------------
# corpora
# ----------------------------------------------------------------------------------------------------------------------
def bc1_corpus(rng, n):
    """-> (n,2) uint32 words and a category name per block."""
    w = np.zeros((n, 2), dtype=np.uint32)
    cat = []
    for i in range(n):
        kind = i % 12
        c0, c1 = int(rng.integers(0, 65536)), int(rng.integers(0, 65536))
        idx = int(rng.integers(0, 2 ** 32))
        if kind == 0:
            name = 'random'
        elif kind == 1:
            name, c1 = 'c0==c1 (3-colour mode, random idx)', c0
        elif kind == 2:
            name, c1 = 'c0==c1 solid (idx all 0)', c0
            idx = 0
        elif kind == 3:
            name = 'opaque c0>c1 random idx'
            c0, c1 = max(c0, c1) | 1, min(c0, c1)
            if c0 == c1:
                c0 += 1
        elif kind == 4:
            name = '3-colour c0<c1, no idx 3'
            c0, c1 = min(c0, c1), max(c0, c1)
            if c0 == c1:
                c1 = min(c1 + 1, 65535)
            idx = int(sum(int(rng.integers(0, 3)) << (2 * t) for t in range(16)))
        elif kind == 5:
            name = '3-colour c0<c1, with transparent'
            c0, c1 = min(c0, c1), max(c0, c1)
            if c0 == c1:
                c1 = min(c1 + 1, 65535)
            idx |= 0x3 << (2 * int(rng.integers(0, 16)))
        elif kind == 6:
            name = 'opaque, full range black/white'
            c0, c1 = (0xFFFF, 0x0000) if i & 1 else (0x0000, 0xFFFF)
            if c0 < c1:
                c0, c1 = c1, c0
        elif kind == 7:
            name = 'opaque, close colours (smooth)'
            base = int(rng.integers(0, 65536 - 40))
            c0, c1 = base + int(rng.integers(1, 40)), base
        elif kind == 8:
            name = 'opaque, all idx the same'
            c0, c1 = max(c0, c1) | 1, min(c0, c1)
            if c0 == c1:
                c0 += 1
            idx = int(rng.integers(0, 4)) * 0x55555555
        elif kind == 9:
            name = 'opaque, gradient idx 0,3,2,1 (ramp)'
            c0, c1 = max(c0, c1) | 1, min(c0, c1)
            if c0 == c1:
                c0 += 1
            idx = int(sum([0, 2, 3, 1][t % 4] << (2 * t) for t in range(16)))
        elif kind == 10:
            name = '3-colour, saturated corners (0 / 0xFFFF)'
            c0, c1 = (0x0000, 0xFFFF)
        else:
            name = 'opaque, one channel extreme'
            c0, c1 = 0xF800 | (c0 & 0x7FF) | 1, c1 & 0x07FF
            if c0 <= c1:
                c0 = c1 + 1
        w[i, 0] = (c0 & 0xFFFF) | ((c1 & 0xFFFF) << 16)
        w[i, 1] = idx & 0xFFFFFFFF
        cat.append(name)
    return w, cat


def bc4_corpus(rng, n):
    w = np.zeros((n, 2), dtype=np.uint32)
    cat = []
    for i in range(n):
        kind = i % 11
        r0, r1 = int(rng.integers(0, 256)), int(rng.integers(0, 256))
        bits = int(rng.integers(0, 2 ** 48))
        if kind == 0:
            name = 'random'
        elif kind == 1:
            name, r1 = 'r0==r1 (6-level mode)', r0
        elif kind == 2:
            name = '8-level (r0>r1) random idx'
            r0, r1 = max(r0, r1), min(r0, r1)
            if r0 == r1:
                r0 = min(r0 + 1, 255)
                r1 = r0 - 1
        elif kind == 3:
            name = '6-level (r0<r1) random idx'
            r0, r1 = min(r0, r1), max(r0, r1)
            if r0 == r1:
                r1 = min(r0 + 1, 255)
        elif kind == 4:
            name = '8-level full range 255/0'
            r0, r1 = 255, 0
        elif kind == 5:
            name = '8-level narrow range (<= 7)'
            r1 = int(rng.integers(0, 248))
            r0 = r1 + int(rng.integers(1, 8))
        elif kind == 6:
            name = 'all texels the same index'
            bits = int(rng.integers(0, 8)) * 0o7777777777777777
            bits &= (1 << 48) - 1
        elif kind == 7:
            name = 'two levels in use (idx 0 / 1 only: masks)'
            r0, r1 = max(r0, r1), min(r0, r1)
            bits = int(sum(int(rng.integers(0, 2)) << (3 * t) for t in range(16)))
        elif kind == 8:
            name = 'ramp idx 0..7 in order, 8-level'
            r0, r1 = max(r0, r1), min(r0, r1)
            order = [0, 2, 3, 4, 5, 6, 7, 1]
            bits = int(sum(order[t % 8] << (3 * t) for t in range(16)))
        elif kind == 9:
            name = '6-level with 0 / 255 (idx 6 / 7) in use'
            r0, r1 = min(r0, r1), max(r0, r1)
            bits = int(sum(int(rng.choice([0, 1, 2, 3, 4, 5, 6, 7])) << (3 * t) for t in range(16)))
        else:
            name = 'mid-range 8-level, wide (64..255)'
            r0, r1 = 255 - int(rng.integers(0, 40)), 64 + int(rng.integers(0, 40))
        w[i, 0] = (r0 & 255) | ((r1 & 255) << 8) | ((bits & 0xFFFF) << 16)
        w[i, 1] = (bits >> 16) & 0xFFFFFFFF
        cat.append(name)
    return w, cat


def stats(err, label, width=44):
    e = np.asarray(err, dtype=np.float64).ravel()
    return f'  {label:<{width}} n={e.size:>7}  max {e.max():7.3f}  mean {e.mean():6.3f}  p99 {np.percentile(e, 99):6.3f}'


# ----------------------------------------------------------------------------------------------------------------------
# tests
# ----------------------------------------------------------------------------------------------------------------------
def make_layout(Wb, Hb, Z, bpb_log2, tiled, three_d):
    """-> (guest bytes needed, guest_pitch for the push constants, z stride rows)."""
    bpb = 1 << bpb_log2
    if tiled:
        pitch_blocks = (Wb + 31) // 32 * 32
        rows = (Hb + 31) // 32 * 32
        if three_d:
            zrows = rows
            return pitch_blocks * rows * ((Z + 3) // 4 * 4) * bpb + 4096, pitch_blocks, zrows
        return pitch_blocks * rows * bpb + 4096, pitch_blocks, 0
    pitch = (Wb + 3) // 4 * 4 * bpb
    return pitch * Hb * Z + 4096, pitch, Hb


def test_addr(rng):
    print('== addr: new shaders (COPY64 / COPY128) vs the shipped texture_load_64bpb / 128bpb ==')
    ok = True
    n = 0
    for bpb_log2, define, shipped in ((3, 'COPY64', 'texture_load_64bpb_cs'), (4, 'COPY128', 'texture_load_128bpb_cs')):
        mine, ship = build_variant(define), shipped_spv(shipped)
        per_thread = 4 if bpb_log2 == 3 else 2
        for (Wb, Hb, Z, tiled, three_d, endian) in [(64, 64, 1, 0, 0, 0), (37, 21, 1, 0, 0, 1), (128, 96, 1, 1, 0, 0),
                                                    (130, 70, 1, 1, 0, 1), (33, 33, 1, 1, 0, 2), (200, 40, 1, 1, 0, 3),
                                                    (64, 64, 5, 1, 1, 0), (40, 36, 6, 1, 1, 2), (50, 20, 3, 0, 1, 0)]:
            gbytes, gpitch, gz = make_layout(Wb, Hb, Z, bpb_log2, tiled, three_d)
            src = rng.integers(0, 256, gbytes, dtype=np.uint8).tobytes()
            bpb = 1 << bpb_log2
            hpitch = bpb * ((Wb + per_thread - 1) // per_thread * per_thread)
            dbytes = hpitch * Hb * Z + 4096
            flags = tiled | (three_d << 1) | (endian << 2)
            push = push_bytes(flags, 0, gpitch, gz, (Wb, Hb, Z), 0, hpitch, Hb * 4)
            groups = ((Wb + (4 * per_thread) - 1) // (4 * per_thread), (Hb + 31) // 32, Z)
            a, _ = run_gpu(mine, groups, push, src, dbytes, False)
            b, _ = run_gpu(ship, groups, push, src, dbytes, True)
            same = a == b and any(a[:4096])
            n += 1
            ok &= same
            print(f'  {define:7} {Wb:3}x{Hb:<3}x{Z} tiled={tiled} 3d={three_d} endian={endian}: '
                  f'{"IDENTICAL" if same else "DIFFERENT"} ({dbytes} bytes)')
    print(f'  -> {"PASS" if ok else "FAIL"} ({n} configs)')
    return ok


def run_transcode(define, words_per_block, blocks_words, Wb, Hb, per_thread, dst_block_bytes, spv=None):
    n_blocks = Wb * Hb
    src = blocks_words.astype('<u4').tobytes()
    src += bytes(4096)
    hpitch = dst_block_bytes * ((Wb + per_thread - 1) // per_thread * per_thread)
    dbytes = hpitch * Hb + 4096
    push = push_bytes(0, 0, Wb * words_per_block * 4, Hb, (Wb, Hb, 1), 0, hpitch, Hb * 4)
    groups = ((Wb + 4 * per_thread - 1) // (4 * per_thread), (Hb + 31) // 32, 1)
    spv = spv or build_variant(define)
    out, us = run_gpu(spv, groups, push, src, dbytes, False)
    # The SHIPPED SPIR-V header of the SDK (the exact bytes the game loads; dst set 0, src set 1) must give the same bytes
    # as the test build of the GLSL (Session 93 review).
    shipped = {'TRANSCODE_BC1_ASTC': 'texture_load_dxt1_astc4x4_cs', 'TRANSCODE_BC1_ASTC,BC1_ALPHA_FIX': 'texture_load_dxt1_astc4x4_alphafix_cs', 'TRANSCODE_BC4_EAC': 'texture_load_dxt5a_eac_r11_cs',
               'TRANSCODE_BC5_EAC': 'texture_load_dxn_eac_rg11_cs'}[define]
    out2, _ = run_gpu(shipped_spv(shipped), groups, push, src, dbytes, True)
    run_transcode.shipped_ok = getattr(run_transcode, 'shipped_ok', True) and out2 == out
    print(f'  shipped header {shipped}: {"IDENTICAL to the test build" if out2 == out else "DIFFERENT from the test build"}')
    buf = np.frombuffer(out2, dtype=np.uint8)
    blocks = [buf[(y * hpitch) + x * dst_block_bytes:(y * hpitch) + (x + 1) * dst_block_bytes] for y in range(Hb) for x in range(Wb)]
    return blocks, us


def test_ref(rng):
    print('== ref: CPU BC decoders vs the shipped DXT1->RGBA8, DXT5A->R8, DXN->RG8 shaders (tolerance 1) ==')
    ok = True
    Wb, Hb = 64, 32
    w1, _ = bc1_corpus(rng, Wb * Hb)
    w4, _ = bc4_corpus(rng, Wb * Hb)
    w5 = np.concatenate([bc4_corpus(rng, Wb * Hb)[0], bc4_corpus(rng, Wb * Hb)[0]], axis=1)
    for name, shader, words, wpb, bytes_per_texel, chans in (('DXT1', 'texture_load_dxt1_rgba8_cs', w1, 2, 4, 4),
                                                              ('DXT5A', 'texture_load_dxt5a_r8_cs', w4, 2, 1, 1),
                                                              ('DXN', 'texture_load_dxn_rg8_cs', w5, 4, 2, 2)):
        spv = shipped_spv(shader)
        info = {'DXT1': 2, 'DXT5A': 2, 'DXN': 1}[name]
        per_thread = 1 << info
        tex_w, tex_h = Wb * 4, Hb * 4
        hpitch = bytes_per_texel * ((tex_w + per_thread * 4 - 1) // (per_thread * 4) * (per_thread * 4))
        src = words.astype('<u4').tobytes() + bytes(4096)
        dbytes = hpitch * tex_h + 4096
        push = push_bytes(0, 0, Wb * wpb * 4, Hb, (Wb, Hb, 1), 0, hpitch, tex_h)
        groups = ((Wb + 4 * per_thread - 1) // (4 * per_thread), (Hb + 31) // 32, 1)
        out, _ = run_gpu(spv, groups, push, src, dbytes, True)
        img = np.frombuffer(out, dtype=np.uint8)[:hpitch * tex_h].reshape(tex_h, hpitch)[:, :tex_w * bytes_per_texel] \
            .reshape(tex_h, tex_w, chans).astype(np.float64)
        worst = 0.0
        mism = 0
        for by in range(Hb):
            for bx in range(Wb):
                i = by * Wb + bx
                if name == 'DXT1':
                    ref = bc1_decode(int(words[i, 0]), int(words[i, 1]))
                elif name == 'DXT5A':
                    ref = bc4_decode(int(words[i, 0]), int(words[i, 1]))[:, None]
                else:
                    ref = np.stack([bc4_decode(int(words[i, 0]), int(words[i, 1])),
                                    bc4_decode(int(words[i, 2]), int(words[i, 3]))], axis=1)
                got = img[by * 4:by * 4 + 4, bx * 4:bx * 4 + 4].reshape(16, chans)
                d = np.abs(got - np.round(ref) if name != 'DXT1' else got - ref)
                if name == 'DXT1':
                    # transparent texels: the shipped shader may write any colour with alpha 0
                    mask = ref[:, 3] == 0
                    d[mask, :3] = 0
                worst = max(worst, d.max())
                mism += int((d > 1.0001).any())
        res = worst <= 1.0001
        ok &= res
        print(f'  {name:6} {Wb*Hb} blocks: max |shipped - CPU reference| = {worst:.3f}, blocks over 1: {mism} -> {"ok" if res else "FAIL"}')
    print(f'  -> {"PASS" if ok else "FAIL"}')
    return ok


S93_BASE = 'a89047e'  # the SDK commit of Session 93 whose DXT1 header the fix-off output must match byte for byte


def s93_dxt1_spv():
    text = subprocess.run(['git', '-C', str(SDK), 'show', f'{S93_BASE}:src/graphics/shaders/vulkan_spirv/texture_load_dxt1_astc4x4_cs.h'],
                          capture_output=True, text=True, check=True).stdout
    body = text.split('[] = {', 1)[1].split('};', 1)[0]
    words = [int(w, 16) for w in re.findall(r'0x[0-9A-Fa-f]{8}', body)]
    spv = WORK / 'shipped_s93_dxt1.spv'
    spv.write_bytes(np.array(words, dtype='<u4').tobytes())
    return spv


def test_bc1_vs_s93(rng, n):
    Wb = 128
    Hb = (n + Wb - 1) // Wb
    n = Wb * Hb
    words, _ = bc1_corpus(rng, n)
    new, _ = run_transcode('TRANSCODE_BC1_ASTC', 2, words, Wb, Hb, 4, 16)
    hpitch = 16 * ((Wb + 3) // 4 * 4)
    src = words.astype('<u4').tobytes() + bytes(4096)
    push = push_bytes(0, 0, Wb * 2 * 4, Hb, (Wb, Hb, 1), 0, hpitch, Hb * 4)
    groups = ((Wb + 15) // 16, (Hb + 31) // 32, 1)
    out, _ = run_gpu(s93_dxt1_spv(), groups, push, src, hpitch * Hb + 4096, True)
    buf = np.frombuffer(out, dtype=np.uint8)
    diff = sum(1 for i, b in enumerate(new) if not np.array_equal(b, buf[(i // Wb) * hpitch + (i % Wb) * 16:(i // Wb) * hpitch + (i % Wb) * 16 + 16]))
    print(f'  fix OFF vs the Session 93 header ({S93_BASE}): {n} blocks, {diff} differ -> {"IDENTICAL" if diff == 0 else "FAIL"}')
    return diff == 0


def test_bc1(rng, n, define='TRANSCODE_BC1_ASTC'):
    print('== bc1: BC1 -> ASTC 4x4 (GPU) decoded by the CPU ASTC subset decoder vs the exact BC1 decode ==')
    Wb = 128
    Hb = (n + Wb - 1) // Wb
    n = Wb * Hb
    words, cat = bc1_corpus(rng, n)
    blocks, us = run_transcode(define, 2, words, Wb, Hb, 4, 16)
    print(f'  GPU {run_gpu.gpu}: dispatch {us:.0f} us for {n} blocks')
    per = {}
    unsupported = 0
    contract = 0
    n_opaque = n_punch = 0
    all_rgb, op_rgb, pt_rgb, pt_alpha_bad, exact8 = [], [], [], 0, 0
    total_texels = 0
    modes = {}
    transp_texels = 0
    tdist = []
    for i in range(n):
        w0, w1 = int(words[i, 0]), int(words[i, 1])
        ref = bc1_decode(w0, w1)
        try:
            dec, con = astc_decode(blocks[i])
        except AstcUnsupported as e:
            unsupported += 1
            print(f'  block {i}: unsupported by the test decoder: {e}')
            continue
        contract += int(con)
        mode = int.from_bytes(bytes(blocks[i][:4]), 'little') & 0x7FF
        modes[mode] = modes.get(mode, 0) + 1
        punch = (w0 & 0xFFFF) <= (w0 >> 16)
        opaque_t = ref[:, 3] == 255
        err = np.abs(dec[:, :3] - ref[:, :3])
        if punch:
            n_punch += 1
            alpha_bad = np.abs(np.round(dec[:, 3]) - ref[:, 3]) > 0  # alpha must be exactly 0 / 255
            pt_alpha_bad += int(alpha_bad.sum())
            transp_texels += int((~opaque_t).sum())
            if opaque_t.any() and (~opaque_t).any():
                mean_op = ref[opaque_t, :3].mean(axis=0)
                tdist.append(np.abs(dec[~opaque_t, :3] - mean_op).max(axis=1))
            e = err[opaque_t].ravel()
            pt_rgb.append(e)
            per.setdefault(cat[i], []).append(e)
        else:
            n_opaque += 1
            op_rgb.append(err.ravel())
            per.setdefault(cat[i], []).append(err.ravel())
        e8 = np.round(dec[:, :3]) == np.round(ref[:, :3])
        exact8 += int(e8[opaque_t].sum())
        total_texels += int(opaque_t.sum()) * 3
        all_rgb.append(err[opaque_t].ravel())
    print(f'  blocks: {n_opaque} opaque-mode (c0 > c1, ASTC block mode 0x042), {n_punch} punch-through mode (c0 <= c1, 0x451 dual plane); '
          f'unsupported {unsupported}; blue contraction used by the decoder: {contract}; block modes seen: '
          f'{ {hex(k): v for k, v in modes.items()} }')
    ok = unsupported == 0 and contract == 0
    e_op = np.concatenate(op_rgb) if op_rgb else np.zeros(1)
    e_pt = np.concatenate(pt_rgb) if pt_rgb else np.zeros(1)
    print(stats(e_op, 'opaque blocks, RGB error (8-bit units)'))
    print(stats(e_pt, 'punch-through blocks, RGB error, opaque texels'))
    print(f'  punch-through alpha: {pt_alpha_bad} texels not exactly 0 / 255 (of {n_punch * 16}); transparent texels {transp_texels} '
          f'(their colour is not compared: BC1 gives (0,0,0), we keep weight-0 colour with alpha 0)')
    print(f'  exact 8-bit matches (rounded decoded == rounded reference, per channel of opaque texels): '
          f'{exact8} of {total_texels} = {100.0 * exact8 / max(total_texels, 1):.1f} %')
    print('  per category (RGB error, opaque texels):')
    for k in sorted(per):
        print(stats(np.concatenate(per[k]), k))
    bar_op = e_op.max() <= 2.0
    bar_pt = e_pt.max() <= 4.0 and pt_alpha_bad == 0
    print(f'  bar opaque max <= 2 LSB: {"PASS" if bar_op else "FAIL"} (measured {e_op.max():.3f}); '
          f'punch-through alpha exact and colour <= 4 LSB: {"PASS" if bar_pt else "FAIL"} (measured {e_pt.max():.3f})')
    td = np.concatenate(tdist) if tdist else np.zeros(1)
    print(f'  transparent texels: |decoded colour - mean opaque colour of the block| (8-bit, max over channels): mean {td.mean():.2f}  p90 {np.percentile(td, 90):.1f}  max {td.max():.1f}  (n={td.size})')
    test_bc1.tdist_mean = float(td.mean())
    return ok and bar_op and bar_pt


def eac_block_stats(rng, n, channels):
    Wb = 128
    Hb = (n + Wb - 1) // Wb
    n = Wb * Hb
    if channels == 1:
        words, cat = bc4_corpus(rng, n)
        blocks, us = run_transcode('TRANSCODE_BC4_EAC', 2, words, Wb, Hb, 4, 8)
    else:
        w_a, cat = bc4_corpus(rng, n)
        w_b, _ = bc4_corpus(np.random.default_rng(int(rng.integers(0, 2 ** 31))), n)
        words = np.concatenate([w_a, w_b], axis=1)
        blocks, us = run_transcode('TRANSCODE_BC5_EAC', 4, words, Wb, Hb, 2, 16)
    print(f'  GPU {run_gpu.gpu}: dispatch {us:.0f} us for {n} blocks')
    per = {}
    allerr = []
    exact = tot = 0
    for i in range(n):
        for ch in range(channels):
            w0, w1 = int(words[i, 2 * ch]), int(words[i, 2 * ch + 1])
            ref = bc4_decode(w0, w1)
            dec = eac_decode(blocks[i][8 * ch:8 * ch + 8])
            err = np.abs(dec - ref)
            allerr.append(err)
            per.setdefault(cat[i], []).append(err)
            exact += int((np.round(dec) == np.round(ref)).sum())
            tot += 16
    e = np.concatenate(allerr)
    print(stats(e, 'all blocks, error (8-bit units)'))
    print(f'  exact 8-bit matches (rounded EAC == rounded BC4): {exact} of {tot} = {100.0 * exact / tot:.1f} %')
    within1 = float((e <= 1.0).mean())
    print(f'  texels within 1 LSB: {100 * within1:.2f} %   within 2 LSB: {100 * float((e <= 2.0).mean()):.2f} %   '
          f'within 4 LSB: {100 * float((e <= 4.0).mean()):.2f} %')
    print('  per category:')
    for k in sorted(per):
        print(stats(np.concatenate(per[k]), k))
    return e


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--blocks', type=int, default=8192)
    ap.add_argument('--seed', type=int, default=93)
    ap.add_argument('--only', default='')
    a = ap.parse_args()
    if not EXE.exists():
        subprocess.run(['pwsh', '-NoProfile', '-File', str(ROOT / 'tools' / 'native' / 'compute_test' / 'build.ps1')], check=True)
    rng = np.random.default_rng(a.seed)
    results = {}
    want = a.only.split(',') if a.only else ['addr', 'ref', 'bc1', 'bc4', 'bc5']
    if 'addr' in want:
        results['addr'] = test_addr(rng)
    if 'ref' in want:
        results['ref'] = test_ref(rng)
    if 'bc1' in want:
        results['bc1'] = test_bc1(np.random.default_rng(a.seed + 7), a.blocks)
        results['bc1_vs_s93'] = test_bc1_vs_s93(np.random.default_rng(a.seed + 7), a.blocks)
        base_td = test_bc1.tdist_mean
        print('== bc1 + BC1_ALPHA_FIX (Session 94): transparent texels take the block mean opaque colour level ==')
        results['bc1_alpha_fix'] = test_bc1(np.random.default_rng(a.seed + 7), a.blocks, 'TRANSCODE_BC1_ASTC,BC1_ALPHA_FIX')
        fix_td = test_bc1.tdist_mean
        better = fix_td <= 0.5 * base_td
        print(f'  bar transparent-texel colour distance to the mean opaque colour at least 2x smaller: {"PASS" if better else "FAIL"} ({fix_td:.2f} vs {base_td:.2f} without the fix)')
        results['bc1_alpha_fix'] = results['bc1_alpha_fix'] and better
    if 'bc4' in want:
        print('== bc4: BC4 (DXT5A) -> EAC R11, decoded by the CPU EAC decoder vs the exact BC4 decode ==')
        e = eac_block_stats(rng, a.blocks, 1)
        results['bc4'] = e.max() <= EAC_BAR_MAX and e.mean() <= EAC_BAR_MEAN
        print(f'  bar max <= {EAC_BAR_MAX} LSB, mean <= {EAC_BAR_MEAN}: {"PASS" if results["bc4"] else "FAIL"} (measured max {e.max():.3f}, p99 {np.percentile(e, 99):.3f}, mean {e.mean():.3f})')
    if 'bc5' in want:
        print('== bc5: BC5 (DXN) -> EAC RG11 (two BC4 halves) ==')
        e = eac_block_stats(rng, a.blocks, 2)
        results['bc5'] = e.max() <= EAC_BAR_MAX and e.mean() <= EAC_BAR_MEAN
        print(f'  bar max <= {EAC_BAR_MAX} LSB, mean <= {EAC_BAR_MEAN}: {"PASS" if results["bc5"] else "FAIL"} (measured max {e.max():.3f}, p99 {np.percentile(e, 99):.3f}, mean {e.mean():.3f})')
    results['shipped_headers'] = getattr(run_transcode, 'shipped_ok', True)
    print('SUMMARY ' + ', '.join(f'{k}={"PASS" if v else "FAIL"}' for k, v in results.items()))
    return 0 if all(results.values()) else 1


if __name__ == '__main__':
    sys.exit(main())
