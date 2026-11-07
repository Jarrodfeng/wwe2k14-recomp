r"""The shader family ranking (Session 95, B2): per family of our own pixel shaders, the GPU ms it costs, from one in-run route.

    py -I tools\native\shader_rank.py RUN [--dir LOGS]

Route: short\s95_shader_rank.txt (Surface: tools\surface\bench.ps1 -Only shaderrank, then --dir <the step folder>). The route runs windows of
12 logged frames in one match: baselines (base1..base4) and, between them, one family of our pixel shaders drawn flat grey (ab_<family>, live cvar
gpu_native_shader_ablate; SDK shader_swap.cpp). A family's cost = the baselines' mean minus its window, for the GPU first-to-last span, the busy time
(span minus GPU idle) and the scene passes (pieces >= 1000 px wide: the 1280x720 passes). It bounds what rewriting that family could give on its pixel side (an upper bound for the opaque ones; NOT a bound for alpha-tested families, whose flat grey also loses the discard-driven early-out and overdraw behaviour): the
vertex work, the fill and the textures' bandwidth stay (a flat grey samples nothing, so the saving includes the texture reads). Only the
shaders our swap replaces through shader_swap::Build are flattened. The own crowd (gpu_native_own_crowd) bypasses Build: "crowd" here is only the 2D sprites and the atlas copy, the crowd proper is NOT MEASURED.
The baseline spread (max - min over the base windows) is the noise (heat, the match moving): a family below it is not measured.
"""
import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gpu_profile as gp  # noqa: E402


def scene_ms(frames):
    """GPU ms a frame in the wide passes (>= 1000 px)."""
    total, n = 0.0, 0
    for v in frames.values():
        if not v['total']:
            continue
        n += 1
        for label, box, ps, draws, ms, before in v['pieces']:
            try:
                w = int(box.split('x')[0])
            except ValueError:
                continue
            if w >= 1000:
                total += ms
    return total / max(1, n)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('run')
    ap.add_argument('--dir', default=str(gp.DEFAULT_LOGS))
    a = ap.parse_args()
    scenes = gp.read(gp.log_path(a.run, a.dir))
    skips = 0
    with open(gp.log_path(a.run, a.dir), encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.search(r"[native][owned-only] frames d+-d+:.*?skipped for no pipeline (d+) /", line)
            if m:
                skips += int(m.group(1))
    rows = {}
    for name, frames in scenes.items():
        n, seg, gap, span, idle, subs = gp.totals(frames)
        if n:
            rows[name] = (n, span, span - idle, scene_ms(frames))
    bases = [r for k, r in rows.items() if k.startswith('base')]
    if not bases:
        sys.exit('shader_rank: no base windows (wrong route?)')
    mean = [sum(r[i] for r in bases) / len(bases) for i in (1, 2, 3)]
    spread = [max(r[i] for r in bases) - min(r[i] for r in bases) for i in (1, 2, 3)]
    print(f'baselines ({len(bases)} windows): span {mean[0]:.2f} ms, busy {mean[1]:.2f}, scene passes {mean[2]:.2f}; '
          f'spread (noise) span {spread[0]:.2f} busy {spread[1]:.2f} scene {spread[2]:.2f}')
    for k, r in rows.items():
        if k.startswith('base'):
            print(f'  {k:14s} {r[0]:3d} fr  span {r[1]:7.3f}  busy {r[2]:7.3f}  scene {r[3]:7.3f}')
    print(f'{"family":14s} {"frames":>6s} {"span":>8s} {"busy":>8s} {"scene":>8s}   saved (ms a frame: span / busy / scene passes) = its pixel-side cost')
    ranked = []
    for k, r in rows.items():
        if k.startswith('ab_'):
            ranked.append((mean[2] - r[3], k, r))
    for scene_saved, k, r in sorted(ranked, reverse=True):
        d = [mean[0] - r[1], mean[1] - r[2], mean[2] - r[3]]
        flag = '' if abs(d[2]) > spread[2] else '  (inside the noise)'
        print(f'{k[3:]:14s} {r[0]:6d} {r[1]:8.3f} {r[2]:8.3f} {r[3]:8.3f}   {d[0]:6.2f} / {d[1]:6.2f} / {d[2]:6.2f}{flag}')
    print("crowd: the own crowd is not measured (its pipeline bypasses shader_swap::Build); the crowd row is only the 2D crowd sprites and the atlas copy.")
    print(f"no-pipeline skips (draws left out for want of a pipeline, the whole run: the periodic [native][owned-only] summaries are not per window): {skips}" + ("  <- ablation windows may be low: compile hitches" if skips else ""))
    print('A family can overlap another in a pass (shared fill): the sum of the families need not equal "all".')


if __name__ == '__main__':
    main()
