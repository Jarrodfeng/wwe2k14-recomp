#!/usr/bin/env python3
r"""Lane PVR's check (Session 103): the per-frame posed vertices go through the frame's geometry ring, not a staged copy per mesh. 0 game runs (offline renderer + front-end, no game).

  py -I tools\scene\pvr_check.py [--out DIR] [--capture NAME] [--frame N]

What it answers (docs\research\s103\UPF.md fix 1; docs\renderer\360_HABITS.md row 11): Renderer::UpdateBuffer made a staging buffer + one vkCmdCopyBuffer + a barrier pair per mesh of every
re-posed model (ModelCache::PosedModel: ring pieces, posed weapons) and per cloth piece (FrontEnd::Record); the Surface's flat 4.0 ms `uploads` span scales with that copy COUNT (~35 us each).
Switch wwe2k14_rdr_posed_ring (rdr_scene::g_posed_ring; the stream-free start stores the cvar; the rig / unit store it directly).
Part 1 (out\tools\pvr_unit.exe, tools\scene\pvr_unit.cpp: a synthetic model re-posed every frame through the REAL FrontEnd + renderer; the captures' ring pieces are at rest and their wrestlers are
holes in the rig, so no capture exercises the producers: [measured, 6 captures, S103 PVR]):
  - old path: one staged buffer copy per mesh per re-posed frame; ring: 0 posed copies, one push per model per frame, 0 overflow; validation 0;
  - the picture of every frame is identical, ring vs old path (the hash of the whole RGBA frame);
  - overflow: a 16 MB posed block (over the 12 MB budget) falls back to the model's own buffer, counted, the picture is still the old path's;
  - a model at rest (no re-pose for 8 frames): ring pushes stop, the picture stays identical; two objects of one model: one push a frame; a cloth piece (the cloth block of Record): 0 cloth copies;
  - the renderer's real room: 14 MB + 6 MB pushed by other producers around a 4 MB posed model: no frame dropped that the old path keeps, counted fallbacks;
  - plant 1 (every ring offset one vertex off), plant 2 (a model reads the previous frame's ring offset and pushes nothing) and plant 3 (a cloth piece does the same) and plant 4 (the room check left out: the heavy frame is dropped) must be caught: the unit exits 0 only when caught.
S103 PF2 (docs\research\s103\RCP.md fix F2): on a unified-memory device a re-posed model is written straight into a mapped copy of its vertex buffer (rdr_scene::g_posed_direct; the unit forces it on this discrete card):
  - the picture of every frame identical to the old path and the ring path (hash of the whole RGBA frame), 0 GPU buffer copies, 0 ring pushes, mapped writes = posed meshes, no copy written while a frame in flight may read it;
  - plant 5 (ONE copy per model: every re-pose finds it in flight: detected, counted as fallbacks), plant 6 (one copy AND the in-flight check left out: the audit counter catches the write), plant 7 (the draws bind the
    wrong copy: the picture differs) must be caught (exit 0 only when a check failed).
Part 2 (frame_rig.exe on a04_entrance, arms ring / old, each on its own rig server): the same capture frame draws a byte-identical picture (first render PNG and the hash of every repeat) with the
switch on and off, with 0 validation errors (the ring path changes nothing where nothing is re-posed).
Exit 0 = all PASS.
"""
import argparse, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
import up_check  # noqa: E402  (frame_rig session + run_arm + load_png)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=None)
    ap.add_argument('--capture', default='a04_entrance')
    ap.add_argument('--frame', type=int, default=60)
    ap.add_argument('--repeat', type=int, default=3)
    args = ap.parse_args()
    main_root = up_check.main_checkout()
    import run_dirs
    out = run_dirs.run_out('PVR_gate', args.out)
    os.makedirs(out, exist_ok=True)
    fails = []

    def check(ok, text):
        print(('PASS' if ok else 'FAIL') + '  ' + text)
        if not ok:
            fails.append(text)

    # ---- part 1: the unit
    unit = os.path.join(ROOT, 'out', 'tools', 'pvr_unit.exe')
    check(os.path.exists(unit), 'pvr_unit.exe is built (tools\\scene\\build_rig.ps1)')
    if os.path.exists(unit):
        r = subprocess.run([unit], capture_output=True, text=True, errors='replace', timeout=600)
        open(os.path.join(out, 'pvr_unit.txt'), 'w', encoding='utf8').write(r.stdout + r.stderr)
        for line in r.stdout.splitlines():
            if line.startswith(('PASS', 'FAIL', 'pvr_unit')):
                print('  unit: ' + line)
        check(r.returncode == 0, 'pvr_unit: every check passed (exit %d)' % r.returncode)
        for plant in (1, 2, 3, 4, 5, 6, 7, 8):
            rp = subprocess.run([unit, '--plant', str(plant)], capture_output=True, text=True, errors='replace', timeout=600)
            open(os.path.join(out, 'pvr_unit_plant%d.txt' % plant), 'w', encoding='utf8').write(rp.stdout + rp.stderr)
            caught = (rp.returncode == 0 and 'FAIL  ' in rp.stdout) if plant != 4 else ('frame_ring_full' in rp.stdout + rp.stderr)   # plant 4: the heavy frame is DROPPED (frame_ring_full) and the unit dies on it
            check(caught, 'pvr_unit --plant %d is CAUGHT (the picture differs from the old path; plant 4: the frame is dropped, frame_ring_full)' % plant)

    # ---- part 2: the rig, ring vs old on a capture
    up_check.ENVS = up_check.ENVS + ('WWE_RDR_POSED_RING', 'WWE_RDR_VALIDATION', 'WWE_RDR_POSED_DIRECT_FORCE')
    capture = os.path.join(main_root, 'out', 'captures', args.capture)
    arms, works = {}, {}
    for tag, env in (('ring', {'WWE_RDR_VALIDATION': '1'}), ('old', {'WWE_RDR_POSED_RING': '0', 'WWE_RDR_VALIDATION': '1'}), ('direct', {'WWE_RDR_POSED_DIRECT_FORCE': '1', 'WWE_RDR_VALIDATION': '1'})):   # S103 PF2: + the direct path (forced on this discrete card)
        res, seq, text, rc, work = up_check.run_arm(capture, [args.frame], os.path.join(out, tag), env, repeat=args.repeat)
        works[tag] = work
        check(rc == 0, '%s arm: frame_rig exit %s' % (tag, rc))
        rp = os.path.join(work, 'ours_%d_report.txt' % args.frame)
        body = open(rp, encoding='utf8', errors='replace').read() if os.path.exists(rp) else ''
        verr = [int(x) for x in re.findall(r'validation_errors (\d+)', body)]
        check(bool(verr) and max(verr) == 0, '%s arm: Vulkan validation errors per panel %s (all 0)' % (tag, verr))
        by = re.search(r'pvr_render \w+ buffers \d+ by_producer posed (\d+) cloth (\d+)', body)
        if tag in ('ring', 'direct'):
            check(by is not None and by.group(1) == '0' and by.group(2) == '0', '%s arm: posed and cloth buffer copies are 0 (%s)' % (tag, str(by.groups()) if by else 'no pvr_render line'))
        arms[tag] = [m.group(1) for m in re.finditer(r'pvr_rep \d+ buffers \d+ by_producer posed \d+ cloth \d+ other \d+ ring_pushes \d+ ring_bytes \d+ ring_overflow \d+ hash ([0-9a-f]+)', body)]
    if 'direct' in arms and 'old' in arms:   # S103 PF2: the same picture with the direct path forced on (the capture's pieces are at rest: nothing is re-posed, so this guards the draw's buffer choice)
        check(len(arms['direct']) == args.repeat - 1 and arms['direct'] == arms['old'], 'direct vs old: the picture of every repeat is identical (%s)' % arms['direct'])
    if 'ring' in arms and 'old' in arms:
        check(len(arms['ring']) == args.repeat - 1 and arms['ring'] == arms['old'], 'ring vs old: the picture of every repeat is identical (%s)' % arms['ring'])
        pa, pb = os.path.join(works['ring'], 'ours_%d_mask511.png' % args.frame), os.path.join(works['old'], 'ours_%d_mask511.png' % args.frame)
        if os.path.exists(pa) and os.path.exists(pb):
            ia, ib = up_check.load_png(pa), up_check.load_png(pb)
            d = 255 if ia.shape != ib.shape else int(abs(ia.astype('int16') - ib.astype('int16')).max())
            check(d == 0, 'ring vs old: the first render is identical (max |diff| %d)' % d)
        else:
            check(False, 'both first-render PNGs exist')
    print('RESULT: %s (%d fails)' % ('PASS' if not fails else 'FAIL', len(fails)))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
