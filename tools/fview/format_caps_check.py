#!/usr/bin/env python3
"""Session 101 lane 200: the format-capability fallbacks' acceptance test (tools\\gate\\lanes\\format_caps.json). 0 game runs: captures through fview.

  py -I tools\\fview\\format_caps_check.py [--root DIR]

Needs out\\renderer\\dev\\fview.exe and the captures in the MAIN checkout's out\\captures (brock_tron, s100_match300). Mask bits: src\\renderer\\format_caps.h.
 1 clean      mask 0: validation 0, no fallback ran (no summary line, or every counter 0)
 2 held       linear bits (0d) with the fallback ON: slots went linear->nearest > 0, illegal 0, validation 0
 3 re-find    the same bits with the fallback OFF (80000000 | 0d): illegal linear sampling > 0 (the #185 class: a format that cannot filter sampled
              linearly), nothing downgraded; with the fallback on the same count is 0
 4 identical  mask 0 and the fallback-off arm present the same frames (this device really can filter: the mask only changes what the renderer believes)
 5 live       the fallback-on arm presents different frames from mask 0 on at least one capture (the fallback is really in the draw path)
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
results = []


def report(name, ok, evidence):
    results.append(ok)
    print(f'{"PASS" if ok else "FAIL"}  {name}: {evidence}', flush=True)


def run(exe, root, cap, mask, hashes):
    cmd = [str(exe), '--root', str(root), '--capture', str(cap), '--replay', '--validation', '1', '--format-caps-mask', mask, '--hash-frames', str(hashes)]
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=900)
    text = p.stdout + p.stderr
    hashes.with_suffix('.log').write_text(text)  # each arm's fview output stays next to its hash CSV
    m = re.search(r'validation errors (\d+)', text)
    s = None
    for s in re.finditer(r'format caps summary: linear->nearest slots (\d+), blits->nearest (\d+), blend off (\d+), vertex unsupported (\d+); illegal \(fallbacks off\) linear (\d+), blend (\d+)', text): pass  # the LAST summary line (first one at the first change, final at shutdown)
    frames = [l for l in hashes.read_text().splitlines() if not l.startswith('format')] if hashes.exists() else []
    caps_lines = sum(1 for l in text.splitlines() if l.startswith('[rdr] format caps:'))
    return (int(m.group(1)) if m else None), (tuple(int(x) for x in s.groups()) if s else (0, 0, 0, 0, 0, 0)), frames, caps_lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=str(HERE.parent.parent))
    args = ap.parse_args()
    root = Path(args.root).resolve()
    sys.path.insert(0, str(Path(__file__).resolve().parent))   # lane 218: the shared locator (refuses an exe older than the renderer sources)
    import fview_exe as fe
    exe = Path(fe.find_or_exit(root=root, kind='dev'))
    main_checkout = root if (root / 'out' / 'captures').exists() else root.parents[2]
    if not exe.exists():
        print(f'FAIL  fview.exe missing: {exe}')
        return 1
    tmp = Path(tempfile.mkdtemp(prefix='format_caps_'))
    any_live = False
    for name in ('brock_tron', 's100_match300'):
        cap = main_checkout / 'out' / 'captures' / name
        if not cap.exists():
            report(f'{name}', False, f'capture missing: {cap}')
            continue
        v0, s0, f0, l0 = run(exe, root, cap, '0', tmp / f'{name}_0.csv')
        von, son, fon, lon = run(exe, root, cap, '0d', tmp / f'{name}_on.csv')
        voff, soff, foff, loff = run(exe, root, cap, '8000000d', tmp / f'{name}_off.csv')
        report(f'{name} clean', v0 == 0 and bool(f0) and s0 == (0, 0, 0, 0, 0, 0) and l0 == 0, f'validation {v0}, counters {s0}, {l0} [rdr] format caps lines at mask 0 (must be 0), {len(f0)} frames')
        report(f'{name} held', von == 0 and son[0] > 0 and son[4] == 0, f'validation {von}, linear->nearest slots {son[0]}, illegal {son[4]}')
        report(f'{name} re-find', voff == 0 and soff[4] > 0 and soff[0] == 0 and son[4] == 0, f'fallback off: illegal linear samples {soff[4]} (on: {son[4]}), downgraded {soff[0]}')
        report(f'{name} identical', f0 == foff and bool(f0), f'{len(f0)} frames, mask 0 vs fallback-off arm: {sum(a != b for a, b in zip(f0, foff))} differ')
        any_live |= f0 != fon
        print(f'      {name}: logs in {tmp}; fallback-on arm differs from mask 0 in {sum(a != b for a, b in zip(f0, fon))} of {len(f0)} frames', flush=True)
    report('live', any_live, 'the fallback-on arm changed the presented frames on at least one capture')
    return 0 if all(results) else 1


if __name__ == '__main__':
    sys.exit(main())
