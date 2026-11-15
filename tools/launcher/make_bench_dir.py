#!/usr/bin/env python3
r"""S102 lane U4: builds the bench\ folder the launcher's BENCHMARK button needs next to the exe (src\launcher\bench_button.h RequiredFiles()).

  py -I tools\launcher\make_bench_dir.py OUT_DIR

Writes OUT_DIR\bench\{brock_entrance,bryan_entrance,match}.txt (the Surface bench's per-scene routes, tools\input_scripts\scenes\, with every `screenshot` line removed
and nothing else changed: a player's run takes no pictures), OUT_DIR\bench\nav\boot_to_menu.txt (the snippet they `include`) and OUT_DIR\bench\states.toml (their wait_state
names; the script reads it next to itself). Called by tools\release\make_release.ps1 -BenchButton (OFF until the button's first real run). Our own scripts only.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCENES = ('brock_entrance', 'bryan_entrance', 'match')


def strip_screenshots(text):
    """Every line whose first word is `screenshot` goes; comments and everything else stay byte for byte."""
    keep = [l for l in text.splitlines(keepends=True) if not re.match(r'\s*screenshot\b', l)]
    return ''.join(keep)


def build(out_dir, root=ROOT):
    out = Path(out_dir) / 'bench'
    (out / 'nav').mkdir(parents=True, exist_ok=True)
    made = []
    for s in SCENES:
        src = root / 'tools' / 'input_scripts' / 'scenes' / f'{s}.txt'
        text = src.open(encoding='utf-8', newline='').read()
        if 'log bench: done' not in text or 'mark 1 ' not in text:
            raise SystemExit(f'FAIL  {src} lacks "log bench: done" or "mark 1": the button could not judge it')
        (out / f'{s}.txt').write_text(strip_screenshots(text), encoding='utf-8', newline='')
        made.append(f'bench/{s}.txt')
    nav = (root / 'tools' / 'input_scripts' / 'nav' / 'boot_to_menu.txt').open(encoding='utf-8', newline='').read()
    if re.search(r'^\s*include\b', nav, re.M):
        raise SystemExit('FAIL  nav/boot_to_menu.txt now includes another file: copy that one too (make_bench_dir.py)')
    (out / 'nav' / 'boot_to_menu.txt').write_text(nav, encoding='utf-8', newline='')
    (out / 'states.toml').write_bytes((root / 'tools' / 'input_scripts' / 'states.toml').read_bytes())
    made += ['bench/nav/boot_to_menu.txt', 'bench/states.toml']
    return made


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    for f in build(sys.argv[1]):
        print('wrote', f)
