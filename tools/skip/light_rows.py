r"""S101 lane 109 (LS2/LS3): the Python port of src\renderer_scene\light_state_rows.h, for the values ledger's `derived` and `light_state` columns.
Float32 arithmetic is emulated by rounding after every operation (struct 'f'), the order of operations is the C++ header's. tools\light\light_states_selftest.py
checks this port against the C++ builder's output on the same states (bit for bit), so the two cannot drift.

  rows_from_state(state)  -> {register: {'v'|'p'|'*': [candidate vec4 as 4-tuples of u32 bits]}}   (stage '*' = both stages)
  derived_c197(cols)      -> the float32 sum of four rgbw rows, added in order 0+1, +2, +3 (bits)
  derived_c199(sum_bits)  -> (m, 1/m, 0, 0) bits, m = the largest component (floor 0)
Nothing here reads game files."""
import struct


def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


def b32(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def fl(bits):
    return struct.unpack('<f', struct.pack('<I', bits))[0]


def sum_rows(cols):
    """cols: four rows of four u32 bit patterns (DifLghCol rows 0-3). The float32 sum, per component, added in order 0+1, +2, +3 -> 4 floats."""
    out = []
    for c in range(4):
        t = f32(fl(cols[0][c]) + fl(cols[1][c]))
        t = f32(t + fl(cols[2][c]))
        t = f32(t + fl(cols[3][c]))
        out.append(t)
    return out


def derived_c197(cols):
    return tuple(b32(x) for x in sum_rows(cols))


def derived_c199(sum_floats):
    m = max(max(sum_floats[0], sum_floats[1]), max(sum_floats[2], sum_floats[3]))
    m = max(m, 0.0)
    inv = f32(1.0 / m) if m > 0.0 else 0.0
    return (b32(m), b32(inv), 0, 0)


def _mul4(a, b):
    r = [0.0] * 16
    for i in range(4):
        for j in range(4):
            s = f32(a[i * 4] * b[j])
            for k in range(1, 4):
                s = f32(s + f32(a[i * 4 + k] * b[k * 4 + j]))
            r[i * 4 + j] = s
    return r


def rows_from_state(s):
    """s: dict(light=[224 floats], shadow_m=[3][16], shadow_f=[16], shadow_x=[2][8], map_wh=(w, h), flags). Returns the candidate table."""
    L = s['light']
    row = lambda o: tuple(b32(x) for x in L[o:o + 4])
    out = {}
    cols = [row(24 + 4 * k) for k in range(5)]
    dirs = [row(4 + 4 * k) for k in range(5)]
    for k in range(5):
        out[k] = {'*': [cols[k]]}          # c0-c3 (VS and PS), c4 (PS DifLghCol row 4 / VS KeyLightColor)
        out[8 + k] = {'*': [dirs[k]]}      # c8-c10 (VS), c8-c12 (PS); c12 = KeyLightVec
    sm = sum_rows(cols[:4])
    out[197] = {'p': [tuple(b32(x) for x in sm)]}
    out[199] = {'p': [derived_c199(sm)]}
    a, b = row(88), row(92)
    out[181] = {'*': [a, b]}                 # which block is Sky and which Ground is [U]: either
    out[182] = {'*': [a, b]}
    if (s['flags'] & 1) and s['map_wh'][0] > 0 and s['map_wh'][1] > 0:
        w, h = s['map_wh']
        bias = [0.5, 0, 0, 0, 0, -0.5, 0, 0, 0, 0, 1.0, 0, f32(0.5 + f32(0.5 / w)), f32(0.5 - f32(0.5 / h)), 0, 1.0]
        p = _mul4(_mul4(bias, s['shadow_m'][2]), s['shadow_m'][0])
        for j in range(4):
            out[68 + j] = {'v': [tuple(b32(p[i * 4 + j]) for i in range(4))]}  # column j of the product
    f = s['shadow_f']
    F = lambda byte: f[(byte - 1104) // 4]
    prm = [(F(1108), F(1112), F(1104), F(1120)), (F(1128), F(1136), F(1140), F(1144)), (F(1148), F(1152), F(1156), F(1160))]
    for i in range(3):
        out[168 + i] = {'p': [tuple(b32(x) for x in prm[i])]}
    return out
