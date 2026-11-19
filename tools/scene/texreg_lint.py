#!/usr/bin/env python3
"""S103 lane TX lint: a texture header word's base address is a CPU view address; it is read ONLY through the walk
(texture_registry.h WalkTextureHandle), which calls FetchToGpuForm (+4 KB for the 0xE0000000 view).
Checks (src/scene_live, src/renderer_scene):
  1. WalkTextureHandle's body calls FetchToGpuForm.
  2. Any other file that reads header words (kHeaderFetch) must also call FetchToGpuForm in the same file.
  3. `& 0xFFFFF000` on a fetch word (f[1], fetch[1]) only in guest_texture.h (ShapeOfFetch, fed by the walk) and the diagnostic print in particle_namer.h.
Usage: py -I tools/scene/texreg_lint.py --root .
"""
import re
import sys
from pathlib import Path

root = Path(sys.argv[sys.argv.index('--root') + 1] if '--root' in sys.argv else '.')
bad = []
reg = (root / 'src/scene_live/texture_registry.h').read_text(encoding='utf-8', errors='replace')
m = re.search(r'inline TexWalk WalkTextureHandle\(.*?\n\}', reg, re.S)
if not m or 'FetchToGpuForm' not in m.group(0):
    bad.append('texture_registry.h: WalkTextureHandle does not call FetchToGpuForm')
allow_mask = {'guest_texture.h', 'particle_namer.h'}
for d in ('src/scene_live', 'src/renderer_scene'):
    for p in sorted((root / d).rglob('*')):
        if p.suffix not in ('.h', '.cpp'):
            continue
        t = p.read_text(encoding='utf-8', errors='replace')
        # A raw diagnostic dump (S103 CTF NoteRefused: prints the words as stored, decodes no base) carries the marker `texreg-lint: raw-dump` on its read.
        if p.name != 'texture_registry.h' and 'kHeaderFetch' in t and 'FetchToGpuForm' not in t and 'texreg-lint: raw-dump' not in t:
            bad.append(f'{p}: reads header words (kHeaderFetch) without FetchToGpuForm')
        if p.name not in allow_mask and re.search(r'(fetch\[1\]|\bf\[1\])\s*&\s*0xFFFFF000', t):
            bad.append(f'{p}: masks a fetch word 1 base address outside the walk / ShapeOfFetch')
for b in bad:
    print('FAIL', b)
print('texreg_lint:', 'FAIL' if bad else 'ok')
sys.exit(1 if bad else 0)
