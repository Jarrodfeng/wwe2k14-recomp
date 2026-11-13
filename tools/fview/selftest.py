#!/usr/bin/env python3
"""Session 101 lane 2: fview's offline acceptance test (registered in tools\\gate\\lanes\\fview.json). 0 game runs: it plays captures through the renderer.

  py -I tools\\fview\\selftest.py [--root DIR] [--keep]

Needs out\\renderer\\dev\\fview.exe (tools\\fview\\build.ps1) and the captures in the MAIN checkout's out\\captures. Every check prints PASS / FAIL with the
evidence; a check that cannot read its input FAILS (never passes on an empty read). Exit code 1 on any FAIL.

 1 replay      s100_wm30_300, s100_heenan_build, s100cards: --validation 1, 0 validation errors, and the report line is present
 2 pixel       s100cards, a far-crowd card pixel: the newest draw that wrote it is effect yAudience, game alpha test ON GEQUAL ~0.753, program ON, no mismatch
 3 planted     the same query with --fx-dir = a copy whose yAudience fragment variants say "alpha_test": false: "alpha test: game ON, program OFF"
 4 ops         --ops lists every op; the count equals the frame file's own op table (read here with struct, not through fview)
 5 save case   --save-case writes picture.png + case.json under out\\cases\\ (in a temp root: nothing lands in the repo)
 6 reload      --reload-test: an effect rebuilt from a pristine copy twice gives the same programs; from a copy with an edited glsl, different ones
"""
import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
# The far-crowd card: capture s100cards, frame FRAME, surface SURFACE (the scene colour), pixel (X, Y). Found from the gallery of frame 1; see docs\\renderer\\FVIEW.md.
CARDS = {'capture': 's100cards', 'frame': 1, 'surface': 0, 'x': 0, 'y': 0}
CARDS_FILE = HERE / 'selftest_cards.json'  # the coordinates (our own data, no game bytes): {"frame":.., "surface":.., "x":.., "y":..}

results = []


def report(name, ok, evidence):
    results.append((name, ok))
    print(f'{"PASS" if ok else "FAIL"}  {name}: {evidence}', flush=True)


def run(cmd, timeout=900):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return p.returncode, p.stdout + p.stderr


def frame_op_count(capture_dir, frame):
    """The op table's length, read straight from the frame file (SectionEntry id 1 = ops), independent of fview."""
    data = Path(capture_dir, 'frames', f'frame_{frame:06d}.bin').read_bytes()
    magic, version, cf, section_count = struct.unpack_from('<IIII', data, 0)
    if magic != 0x52464352:
        raise RuntimeError('not a frame file: ' + str(capture_dir))
    for s in range(section_count):
        sid, esize, count, offset = struct.unpack_from('<IIQQ', data, 32 + 24 * s)
        if sid == 1:
            return count
    raise RuntimeError('no op table')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=str(HERE.parent.parent))
    ap.add_argument('--keep', action='store_true')
    args = ap.parse_args()
    root = Path(args.root).resolve()
    sys.path.insert(0, str(Path(__file__).resolve().parent))   # lane 218: the shared locator (refuses an exe older than the renderer sources)
    import fview_exe as fe
    exe = Path(fe.find_or_exit(root=root, kind='dev'))
    main_checkout = root
    if not (root / 'out' / 'captures').exists():  # a worktree: the main checkout is three folders up (.claude\\worktrees\\NAME)
        main_checkout = root.parents[2]
    caps = main_checkout / 'out' / 'captures'
    if not exe.exists():
        print(f'FAIL  fview.exe missing: {exe} (run tools\\fview\\build.ps1)')
        return 1
    tmp = Path(tempfile.mkdtemp(prefix='fview_selftest_'))
    base = [str(exe), '--root', str(root)]

    # 1 replay
    for name in ('s100_wm30_300', 's100_heenan_build', 's100cards'):
        d = caps / name
        if not d.exists():
            report(f'replay {name}', False, f'capture folder not found: {d}')
            continue
        code, out = run(base + ['--capture', str(d), '--validation', '1'])
        ok = code == 0 and 'validation errors 0' in out and '[fview] capture:' in out and 'validation active: yes' in out
        report(f'replay {name}', ok, f'exit {code}; ' + next((l for l in out.splitlines() if 'validation errors' in l), 'NO validation line in output: ' + out[:200]))

    # 2/3 pixel history, real and planted
    cards = caps / CARDS['capture']
    cfg = json.loads(CARDS_FILE.read_text()) if CARDS_FILE.exists() else None
    q, real_top_op, real_after = None, -1, None
    if cfg is None:
        report('pixel cards', False, f'{CARDS_FILE} missing (the far-crowd pixel coordinates)')
    else:
        q = base + ['--validation', '1', '--capture', str(cards), '--frame', str(cfg['frame']), '--pixel', f'{cfg["x"]},{cfg["y"]}', '--surface', str(cfg['surface']), '--through-op', str(cfg['through_op'])]
        real_json = tmp / 'real.json'
        code, out = run(q + ['--json', str(real_json)])
        try:
            h = json.loads(real_json.read_text())
            top = h['entries'][h['top_wrote_index']]
            draw = top['draw']
            a = draw['alpha_test']
            ok = (code == 0 and 'validation active: yes' in out and 'validation errors 0' in out and top['effect'] == 'yAudience' and a['game'] and a['func'] == 'GEQUAL' and abs(a['ref'] - 0.753) < 0.01 and a['program'] and not a['mismatch'])
            ev = f'top wrote op {top["op"]} effect {top["effect"]}, alpha game={a["game"]} func={a["func"]} ref={a["ref"]:.3f} program={a["program"]} twin_used={a["twin_used"]}, {h["entry_count"]} ops touch the pixel'
        except Exception as e:  # unreadable output is a failure, never a pass
            ok, ev = False, f'could not read the result ({e!r}); output: {out[:300]}'
        report('pixel cards: yAudience, alpha GEQUAL 0.753, program ON', ok, ev)
        report('pixel cards: no mismatch line with the real effect folder', 'game ON, program OFF' not in out and 'alpha test: game ON, program ON' in out, 'stdout has "program ON" and no "program OFF"')

        real_top_op = top['op'] if ok else -1
        real_after = top.get('after') if ok else None
        fx_copy = tmp / 'fx_planted'
        shutil.copytree(root / 'src' / 'renderer' / 'shaders' / 'fx', fx_copy)
        jp = fx_copy / 'yAudience.fx.json'
        spec = json.loads(jp.read_text(encoding='utf-8'))
        for v in spec['fragment']:
            v['alpha_test'] = False
        jp.write_text(json.dumps(spec, indent=1), encoding='utf-8')
        # 3a: only the DESCRIPTION says false; the binder still binds the compiled twin: program ON, and the line names the stale table.
        planted_json = tmp / 'planted.json'
        code, out = run(q + ['--fx-dir', str(fx_copy), '--json', str(planted_json)])
        try:
            h = json.loads(planted_json.read_text())
            a = h['entries'][h['top_wrote_index']]['draw']['alpha_test']
            ok = 'STALE TABLE' in out and a['stale_table'] and a['program'] and not a['mismatch'] and a['fx_json_alpha_test'] is False
            ev = f'stale_table={a["stale_table"]} program={a["program"]} fx_json_alpha_test={a["fx_json_alpha_test"]}; "STALE TABLE" in stdout: {"STALE TABLE" in out}'
        except Exception as e:
            ok, ev = False, f'could not read the result ({e!r}); output: {out[:300]}'
        report('description disagreeing with the compiled table is named STALE TABLE (binder unchanged: program ON)', ok, ev)
        # 3b / c: the planted fault goes THROUGH THE BINDER: the effect is rebuilt from the planted folder (no alpha twin), so the binder binds the plain program.
        planted_json2 = tmp / 'planted_binder.json'
        code, out = run(q + ['--apply-reload', 'yAudience', '--fx-dir', str(fx_copy), '--json', str(planted_json2)], timeout=1800)
        try:
            h = json.loads(planted_json2.read_text())
            e = next(e for e in h['entries'] if e['op'] == real_top_op)
            ok = (code == 0 and real_top_op >= 0 and 'program OFF' in e['alpha_line'] and 'STALE' not in e['alpha_line'] and 'alpha test: game ON, program OFF' in out and 'applied reload of yAudience' in out)
            ev = f'op {real_top_op}: "{e["alpha_line"]}"'
        except Exception as e:
            ok, ev = False, f'could not read the result ({e!r}); output: {out[:300]}'
        report('planted fault through the binder (reload without the alpha twin) reads: game ON, program OFF', ok, ev)

    # 4 ops
    ops_json = tmp / 'ops.json'
    frame = 1 if (cards / 'frames' / 'frame_000001.bin').exists() else 0
    code, out = run(base + ['--validation', '1', '--capture', str(cards), '--frame', str(frame), '--ops', '--json', str(ops_json)])
    try:
        ops = json.loads(ops_json.read_text())
        want = frame_op_count(cards, frame)
        kinds = ops['kinds']
        fo = [o['frame_ops'] for o in ops['ops']]
        mono = all(b >= a for a, b in zip(fo, fo[1:]))
        ok = (code == 0 and ops['op_count'] == want == len(ops['ops']) and sum(kinds.values()) == want and kinds['draw'] > 0 and 'validation errors 0' in out
              and mono and fo[-1] == ops['renderer_op_count'] > 0)
        ev = f'fview lists {len(ops["ops"])} ops, the frame file\'s op table has {want}; kinds {kinds}; frame_ops never decreases={mono}, last {fo[-1]} = renderer ops {ops["renderer_op_count"]}'
    except Exception as e:
        ok, ev = False, f'could not read the result ({e!r}); output: {out[:300]}'
    report('ops: every op listed, count = the frame\'s op table, frame_ops monotonic and complete', ok, ev)

    # 5 save case (in a temp root so nothing lands in the repo)
    case_root = tmp / 'caseroot'
    (case_root / 'tools').mkdir(parents=True)
    pix = ['--pixel', f'{cfg["x"]},{cfg["y"]}'] if cfg else []
    surf = ['--surface', str(cfg['surface']), '--through-op', str(cfg['through_op'])] if cfg and cfg['surface'] else []
    code, out = run([str(exe), '--root', str(case_root), '--fx-dir', str(root / 'src' / 'renderer' / 'shaders' / 'fx'), '--validation', '1', '--capture', str(cards), '--frame', str(cfg['frame'] if cfg else frame)] + pix + surf + ['--save-case'])
    dirs = list((case_root / 'out' / 'cases').glob('fview_s100cards_*')) if (case_root / 'out' / 'cases').exists() else []
    try:
        d = dirs[0]
        png = (d / 'picture.png').read_bytes()
        cj = json.loads((d / 'case.json').read_text())
        ok = code == 0 and 'validation errors 0' in out and png[:8] == b'\x89PNG\r\n\x1a\n' and len(png) > 1000 and (case_root / 'out' / 'cases' / 'KEEP').exists() and cj['capture'] and 'op' in cj and ('history' in cj or not pix)
        ev = f'{d.name}: picture.png {len(png)} bytes, case.json keys {sorted(cj)[:8]}'
    except Exception as e:
        ok, ev = False, f'no case written ({e!r}); output: {out[:300]}'
    report('save case: picture.png + case.json under out\\cases', ok, ev)

    # 6 reload
    def reload_hashes(fx_dir, tag):
        j = tmp / f'reload_{tag}.json'
        code, out = run(base + ['--validation', '1', '--capture', str(cards), '--frame', str(frame), '--reload-test', 'yAudience', '--fx-dir', str(fx_dir), '--json', str(j)], timeout=1200)
        r = json.loads(j.read_text())
        return code, r, out
    try:
        pristine = tmp / 'fx_pristine'
        shutil.copytree(root / 'src' / 'renderer' / 'shaders' / 'fx', pristine)
        edited = tmp / 'fx_edited'
        shutil.copytree(root / 'src' / 'renderer' / 'shaders' / 'fx', edited)
        frag = edited / 'yAudience.frag'
        text = frag.read_text(encoding='utf-8')
        needle = 'vec4 diffuse = texture(textures[0], inUv.xy);'
        if needle not in text:
            raise RuntimeError('the edit anchor is not in yAudience.frag any more: ' + needle)
        frag.write_text(text.replace(needle, 'vec4 diffuse = texture(textures[0], inUv.xy) * 0.5;'), encoding='utf-8')
        c1, r1, o1 = reload_hashes(pristine, 'a')
        c2, r2, o2 = reload_hashes(pristine, 'b')
        c3, r3, o3 = reload_hashes(edited, 'c')
        h1 = {v['name']: (v['after'], v['after_alpha']) for v in r1['variants']}
        h2 = {v['name']: (v['after'], v['after_alpha']) for v in r2['variants']}
        h3 = {v['name']: (v['after'], v['after_alpha']) for v in r3['variants']}
        orig = {v['name']: (v['before'], v['before_alpha']) for v in r1['variants']}
        frag_changed = [n for n in h1 if n.endswith('_fragment') and h1[n] != h3.get(n)]
        ok = (c1 == c2 == c3 == 0 and r1['ok'] and r3['ok'] and len(h1) > 5 and h1 == h2 and len(frag_changed) >= 1
              and all(h1[n] == h3[n] for n in h1 if n.endswith('_vertex')) and h1 == orig
              and all('validation errors 0' in o for o in (o1, o2, o3)))
        ev = (f'{len(h1)} variants; rebuilt twice from the pristine copy: identical={h1 == h2}; the pristine rebuild matches the compiled programs={h1 == orig}; '
              f'edited glsl changed {len(frag_changed)} fragment variants (vertex variants unchanged); {r3["seconds"]:.1f} s')
    except Exception as e:
        ok, ev = False, f'reload test could not run ({e!r})'
    report('reload: edited glsl rebuilds to different programs, pristine is repeatable and equals the compiled ones', ok, ev)

    # 6b: render after the reload (under validation): the card pixel keeps its value with the pristine rebuild and changes with the edit
    if cfg is not None and real_after is not None:
        try:
            vals = {}
            for tag, folder in (('pristine', pristine), ('edited', edited)):
                j = tmp / f'after_{tag}.json'
                code, out = run(q + ['--apply-reload', 'yAudience', '--fx-dir', str(folder), '--json', str(j)], timeout=1800)
                h = json.loads(j.read_text())
                e = next(e for e in h['entries'] if e['op'] == real_top_op)
                vals[tag] = (code, e.get('after'), 'validation errors 0' in out)
            ok = (vals['pristine'][0] == vals['edited'][0] == 0 and vals['pristine'][1] == real_after and vals['edited'][1] != real_after and vals['pristine'][2] and vals['edited'][2])
            ev = f'card pixel after op {real_top_op}: compiled {real_after}, pristine rebuild {vals["pristine"][1]}, edited rebuild {vals["edited"][1]}'
        except Exception as e:
            ok, ev = False, f'render after reload could not run ({e!r})'
        report('reload renders: the edit changes the card pixel, the pristine rebuild does not (validation clean)', ok, ev)

    # 7 finding 1: the same op reached two ways gives the same surface. (wm30, frame 1, the scene colour)
    wm = caps / 's100_wm30_300'
    try:
        def surface_hash(args, tag):
            code, out = run(base + ['--capture', str(wm), '--frame', '1', '--surface', '2182', '--surface-hash', tag] + args)
            line = next(l for l in out.splitlines() if '[fview] surface' in l)
            return line.split('hash ')[1].split(' ')[0]
        fresh = surface_hash([], '300')
        stepped = surface_hash([], '300,1269')
        other = surface_hash([], '700')
        ok = fresh == stepped and fresh != other
        ev = f'op 300 fresh {fresh}, reached after going to op 1269 first {stepped}, op 700 {other} (must differ: the hash sees the surface)'
    except Exception as e:
        ok, ev = False, f'surface hash could not run ({e!r})'
    report('surface after op N is the same fresh or after a later op (no drift from earlier renders)', ok, ev)

    # 8 names: a capture without names is shown as such (lane 1's role / provenance readers)
    try:
        code, out = run(q + ['--json', str(tmp / 'names.json')])
        ok = 'role unnamed (no names in this capture)' in out and 'provenance unnamed (no names in this capture)' in out
        ev = 'the draw text says "unnamed (no names in this capture)" for role and provenance' if ok else 'missing in: ' + out[-300:]
    except Exception as e:
        ok, ev = False, repr(e)
    report('names: an old capture reads "unnamed (no names in this capture)"', ok, ev)

    # 9 --scale other than 1 is refused where pixels are the game's
    code, out = run(base + ['--capture', str(cards), '--pixel', '10,10', '--scale', '0.5'])
    report('--scale 0.5 with --pixel is refused', code == 2 and '--scale' in out, f'exit {code}')

    # 10 lane 164: --const-override (test only). s100_match300 has wrestler draws (yCh_*).
    wr = caps / 's100_match300'
    if not wr.exists():
        report('const-override', False, f'capture folder not found: {wr}')
    else:
        def arm(name, extra):
            out_dir = tmp / ('co_' + name)
            code, out = run(base + ['--capture', str(wr), '--frame', '0'] + extra + ['--judge-out', str(out_dir)])
            png = (out_dir / 'picture.png').read_bytes() if (out_dir / 'picture.png').exists() else b''
            line = next((l for l in out.splitlines() if l.startswith('[fview] const-override:')), '')
            return code, out, png, line
        def changed(line):
            try:
                return int(line.split('wrestler draws,')[1].split('draws changed')[0])
            except Exception:
                return -1
        c0, o0, base_png, _ = arm('base', [])
        c1, o1, noop_png, l1 = arm('noop', ['--const-override', 'ps:254.x=7'])
        report('const-override: a register no draw reads changes 0 draws, says NO-OP, and the picture is identical',
               c0 == 0 and c1 == 0 and base_png != b'' and changed(l1) == 0 and 'NO-OP' in o1 and noop_png == base_png, f'exit {c0}/{c1}; {l1}')
        c2, o2, light_png, l2 = arm('light', ['--const-override', 'ps:0-4=0'])
        report('const-override: zeroing the light registers changes wrestler draws and moves pixels', c2 == 0 and changed(l2) > 0 and light_png != b'' and light_png != base_png, f'exit {c2}; {l2}; picture differs: {light_png != base_png}')
        c3, o3, _, l3 = arm('all', ['--const-override', 'ps:0-4=0@class=all'])
        report('const-override: @class=all reaches more draws than the default (wrestlers only)', c3 == 0 and changed(l3) > changed(l2) > 0, f'default {changed(l2)} draws, @class=all {changed(l3)}')
        c4, o4, drop_png, l4 = arm('drop', ['--draw-drop', '@class=wrestlers'])
        report('draw-drop: leaving out the wrestler draws changes the picture and counts the drops', c4 == 0 and drop_png != base_png and 'draw-drop' in o4 and 'draws dropped' in o4, f'exit {c4}; ' + next((l for l in o4.splitlines() if l.startswith('[fview] draw-drop')), 'no draw-drop line'))
        bad = ['ps:170.q=1', 'xx:1=1', 'ps:300=1', 'ps:170.w=1,2', 'ps:170.w', 'ps:170.w=abc', 'ps:170.w=1@class=nope', 'ps:170.w=1@group=x']
        fails = []
        for spec in bad:
            code, out = run(base + ['--capture', str(wr), '--frame', '0', '--const-override', spec, '--ops'])
            if code != 2 or 'bad spec' not in out:
                fails.append(f'{spec} -> exit {code}')
        report('const-override: malformed specs fail cleanly (exit 2, "bad spec")', not fails, f'{len(bad)} specs tried' + (('; wrong: ' + ', '.join(fails)) if fails else ''))

    if not args.keep:
        shutil.rmtree(tmp, ignore_errors=True)
    failed = [n for n, ok in results if not ok]
    print(f'\nfview selftest: {len(results) - len(failed)} passed, {len(failed)} failed')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
