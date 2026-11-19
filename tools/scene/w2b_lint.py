"""S103 lane W2B lint (reviews A and C): the places that read a record object's palette as the SKELETON must use the body prefix (BodyRowCount), and every caster loop that skins a chunk must skip an
absent piece's zero rows (SkinChunkAbsent).
  py -I tools\\scene\\w2b_lint.py [--plant body_count|chunk_absent]   (a plant removes one use and must fail)
Sites: WrestlerCore::BeginFrame (the link needs the 109-node count, not body + spring-hair rows), caster_core.h (two palette reads), rf_wrestlers.cpp (the CAW palette probe), and the five loops
live_map_chain.cpp (MeasureRimDepth), live_map_plane.cpp, live_map_rim.cpp, live_map_shadow.cpp (two).
"""
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
plant = sys.argv[sys.argv.index('--plant') + 1] if '--plant' in sys.argv else ''
need = [
    ('src/renderer_scene/wrestler_core.h', 'BodyRowCount(', 1, 'body_count'),
    ('src/scene_live/caster_core.h', 'BodyRowCount(', 2, 'body_count'),
    ('src/renderer_scene/rf_wrestlers.cpp', 'BodyRowCount(', 1, 'body_count'),
    ('src/renderer_scene/live_map_chain.cpp', 'SkinChunkAbsent(', 1, 'chunk_absent'),
    ('src/renderer_scene/live_map_plane.cpp', 'SkinChunkAbsent(', 1, 'chunk_absent'),
    ('src/renderer_scene/live_map_rim.cpp', 'SkinChunkAbsent(', 1, 'chunk_absent'),
    ('src/renderer_scene/live_map_shadow.cpp', 'SkinChunkAbsent(', 2, 'chunk_absent'),
]
bad = 0
for rel, needle, n, kind in need:
    text = (ROOT / rel).read_text(encoding='utf-8', errors='replace')
    have = text.count(needle)
    if plant == kind:
        have = 0
    if have < n:
        print(f'FAIL {rel}: {needle} x{have}, need {n}')
        bad += 1
print(f'w2b_lint: {"FAILED" if bad else "ok"} ({bad} problem(s))')
sys.exit(1 if bad else 0)
