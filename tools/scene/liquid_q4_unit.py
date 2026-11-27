"""LIQUID4BIT host unit (S111): one liquid-map texel under a sweat item, frame by frame. docs\\systems\\liquid_map.md section 5a.

game: the map texture is k_4_4_4_4; each frame the node pastes it into an 8-bit surface (k_8_8_8_8), blends the item
      (SrcAlpha / InvSrcAlpha) there at 8 bits, and resolves the surface back into the 4-bit texture (round to nearest,
      as the SDK's emulated resolve packs x * 15 + 0.5; a truncating resolve is shown too, as the other bound).
ours before S111: one RGBA8 image, the same blend, stored at 8 bits. Ours after: the game's form (LiveMapsHook::RecordSweat:
      after the items, the variant yLiquidMap_copy_prev_q4 copies each rect back into the RGBA8 map with every channel
      rounded to floor(x * 15 + 0.5) / 15, which is 17 n / 255 exactly at 8 bits).
The drying of a created wrestler (FX_DIM, copy_prev_dim: G / B x 0.775 inside the rects first) is modelled too: with no new
      sweat on a texel (a = 0), the 4-bit map stops drying at 2 / 15 under round to nearest (0.775 x 2 / 15 rounds back to
      2 / 15) and dries to 0 under truncation: there the rounding rule decides the direction.
The sweat channel (G): new = a * amount + (1 - a) * old, a = amount * w (yLiquidMap.frag FX_SWEAT, DstChan 1).
Run: py -I tools\\scene\\liquid_q4_unit.py   (prints the table, exits 0 when every self-check holds)
"""
import math
import sys


def q(x, levels, mode="nearest"):
    x = min(max(x, 0.0), 1.0)
    if mode == "nearest":
        return math.floor(x * levels + 0.5) / levels
    return math.floor(x * levels) / levels


def run(amount, w, frames, store):
    a = amount * w
    v = 0.0
    out = {}
    for f in range(1, frames + 1):
        e = q(a * amount + (1 - a) * v, 255)   # the blend into an 8-bit surface
        v = store(e)
        out[f] = v
    return out


def game_near(e):
    return q(e, 15, "nearest")


def game_trunc(e):
    return q(e, 15, "trunc")


def ours_8bit(e):
    return e


def main():
    frames = 600
    marks = (1, 10, 30, 60, 300, 600)
    print("amount  w     | 8-bit store at frames %s | 4-bit nearest | 4-bit truncate" % (marks,))
    for w in (1.0, 0.5, 0.25):
        for amount in (0.05, 0.10, 0.15, 0.18, 0.20, 0.25, 0.30, 0.40, 0.50, 0.75, 1.00):
            o = run(amount, w, frames, ours_8bit)
            g = run(amount, w, frames, game_near)
            t = run(amount, w, frames, game_trunc)
            fmt = lambda r: " ".join("%.3f" % r[m] for m in marks)
            print("%.2f   %.2f  | %s | %s | %s" % (amount, w, fmt(o), fmt(g), fmt(t)))
        print()
    print("4-bit (nearest) stays 0 for ever when amount^2 * w < 1/30: amount < %.3f at w = 1, %.3f at w = 0.5, %.3f at w = 0.25"
          % (math.sqrt(1 / 30), math.sqrt(1 / 15), math.sqrt(2 / 15)))
    fails = []
    if run(0.15, 1.0, 600, game_near)[600] != 0.0:
        fails.append("faint sweat (0.15) must stay dry in the 4-bit map")
    if not run(0.15, 1.0, 600, ours_8bit)[600] > 0.10:
        fails.append("the 8-bit store must hold faint sweat (the difference this lane fixes)")
    if run(0.20, 1.0, 600, game_near)[600] != 1 / 15:
        fails.append("0.20 at full strength must stall at one 4-bit step")
    if not (run(1.0, 1.0, 10, game_near)[10] == 1.0 and run(1.0, 1.0, 10, ours_8bit)[10] == 1.0):
        fails.append("full sweat must saturate both stores")
    for v in run(0.6, 0.7, 100, game_near).values():
        if abs(v * 15 - round(v * 15)) > 1e-9:
            fails.append("a 4-bit value off the 1/15 lattice: %r" % v)
            break
    # the shader's rounding (yLiquidMap.frag FX_Q4) on every 8-bit input: never a tie, always the 4-bit nearest, and stored back at 8 bits as 17 n exactly
    for n in range(256):
        x = n / 255.0
        k = math.floor(x * 15 + 0.5)
        if abs(x * 15 - (k - 0.5)) < 1e-6 or abs(x * 15 - k) > 0.5 or round(k / 15.0 * 255.0) != 17 * k:
            fails.append("the FX_Q4 rounding of 8-bit value %d is not the 4-bit nearest stored as 17 n" % n)
            break

    def dry(v, frames, mode):   # the dim pass alone (a = 0): T' = Q4(Q8(0.775 x T))
        for _ in range(frames):
            v = q(q(0.775 * v, 255), 15, mode)
        return v
    print("drying (dim, no new sweat), from 15/15 after 600 frames: nearest %.3f, truncate %.3f" % (dry(1.0, 600, "nearest"), dry(1.0, 600, "trunc")))
    if dry(1.0, 600, "nearest") != 2 / 15 or dry(1.0, 600, "trunc") != 0.0:
        fails.append("the 4-bit drying must stop at 2/15 under round to nearest and reach 0 under truncation")
    for f in fails:
        print("FAIL  " + f)
    print("FAIL  liquid_q4_unit" if fails else "PASS  liquid_q4_unit: every check passed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
