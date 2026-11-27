"""S111 TOOLFIX3 (SOCKSHADOW review): the wrestler caster loops all apply the piece rule.
  py -I tools\\lint\\caster_loop_lint.py [--root ROOT] [--selftest]

A wrestler caster loop (shadow A / B, rim, plane, the measures) walks a character's meshes and must skip a chunk that skins to an absent piece (rdr_scene::SkinChunkAbsent) and a piece
the game hides or detached (LiveMapsHook::CasterMeshCasts). Two checks over src\\ (code only, comments stripped):
  1. the CasterMeshCasts( call sites per file equal the known set below (a new sixth loop, or a removed one, changes the count and fails: add the file and its count here once the new loop is reviewed);
  2. every SkinChunkAbsent( call has a CasterMeshCasts( call within three lines either side (the two rules sit together in each loop; either order) (a loop with the chunk rule and no piece rule fails).
Known loops: live_map_shadow.cpp RecordShadow + MeasureCasters (2), live_map_rim.cpp RecordRim (1), live_map_chain.cpp MeasureRimDepth (1), live_map_plane.cpp RecordPlane (1).
The planted twins (run every time, in memory): a loop with the piece rule deleted, and a sixth loop that skips it, must both fail. Exit 0 pass, 1 a finding, 2 could not run."""
import pathlib
import re
import sys

KNOWN = {
    'src/renderer_scene/live_map_shadow.cpp': 2,
    'src/renderer_scene/live_map_rim.cpp': 1,
    'src/renderer_scene/live_map_chain.cpp': 1,
    'src/renderer_scene/live_map_plane.cpp': 1,
}
CALL = re.compile(r'(?<![\w:])(?:\w+::)*CasterMeshCasts\(')
SKIN = re.compile(r'\bSkinChunkAbsent\(')
DEFN = re.compile(r'\b(?:bool|inline)\s+(?:\w+::)*(?:CasterMeshCasts|SkinChunkAbsent)\(')


def code_lines(text):
    out = []
    for ln in text.splitlines():
        out.append(ln.split('//', 1)[0])
    return out


def check(files):
    """files: {rel path: text} -> list of problem strings."""
    bad = []
    for rel, text in sorted(files.items()):
        lines = code_lines(text)
        calls = [i for i, ln in enumerate(lines) if CALL.search(ln) and not DEFN.search(ln) and 'continue' in ln]
        have = len(calls)
        want = KNOWN.get(rel, 0)
        if have != want:
            bad.append('%s: CasterMeshCasts( caller loops x%d, the known set says %d (a new / removed caster loop: review the piece rule, then update KNOWN in tools\\lint\\caster_loop_lint.py)' % (rel, have, want))
        for i, ln in enumerate(lines):
            if SKIN.search(ln) and not DEFN.search(ln) and not any(CALL.search(lines[j]) for j in range(max(0, i - 3), min(len(lines), i + 4))):
                bad.append('%s:%d: a SkinChunkAbsent( loop with no CasterMeshCasts( beside it (the piece rule: a hidden / detached piece casts nothing)' % (rel, i + 1))
    return bad


def load(root):
    files = {}
    for p in sorted((root / 'src').rglob('*')):
        if p.suffix in ('.cpp', '.h', '.inc'):
            t = p.read_text(encoding='utf-8', errors='replace')
            if 'CasterMeshCasts(' in t or 'SkinChunkAbsent(' in t:
                files[p.relative_to(root).as_posix()] = t
    return files


def selftest(files):
    ok = True
    if check(files):
        print('selftest: the real tree must be clean first'); return False
    rim = 'src/renderer_scene/live_map_rim.cpp'
    lines = files[rim].splitlines()
    gone = [ln for ln in lines if not (CALL.search(ln.split('//', 1)[0]) and 'continue' in ln)]
    twin1 = dict(files); twin1[rim] = '\n'.join(gone)
    if not check(twin1):
        print('selftest FAIL: a loop with the piece rule deleted was not caught'); ok = False
    twin2 = dict(files)
    twin2['src/renderer_scene/live_map_sixth.cpp'] = 'for (auto& m : meshes) {\n  if (m.chunk && rdr_scene::SkinChunkAbsent(m.nodes, skin, n)) continue;\n  Draw(m);\n}\n'
    if not check(twin2):
        print('selftest FAIL: a sixth loop with the chunk rule and no piece rule was not caught'); ok = False
    twin3 = dict(files)
    twin3['src/renderer_scene/live_map_sixth.cpp'] = 'for (auto& m : meshes) {\n  if (!CasterMeshCasts(frame, 0, m)) continue;\n  Draw(m);\n}\n'
    if not check(twin3):
        print('selftest FAIL: a sixth loop not in the known set was not caught'); ok = False
    return ok


def main():
    root = pathlib.Path(sys.argv[sys.argv.index('--root') + 1]) if '--root' in sys.argv else pathlib.Path(__file__).resolve().parents[2]
    if not (root / 'src').is_dir():
        print('caster_loop_lint: no src\\ under %s' % root); return 2
    files = load(root)
    for rel in KNOWN:
        if rel not in files:
            print('caster_loop_lint: could not find %s' % rel); return 2
    bad = check(files)
    for b in bad:
        print('FAIL ' + b)
    ok = selftest(files) if not bad else True
    print('caster_loop_lint: %s (%d problem(s))' % ('FAILED' if bad or not ok else 'ok', len(bad) + (0 if ok else 1)))
    return 1 if bad or not ok else 0


if __name__ == '__main__':
    sys.exit(main())
