#!/usr/bin/env python3
"""Lane 14 acceptance test (offline, 0 game runs): scene records as files.

  py -I tools\\scene\\rsc_selftest.py [--root DIR] [--arena 5] [--wrestler 107] [--size 640x360]

Needs out\\tools\\wwe2k14_assets_rdr.exe (tools\\assets\\build_viewer.ps1 -Backend rdr) and out\\tools\\rsc_dump.exe + rsc_draw.exe
(tools\\scene\\build_rsc.ps1) under --root. Output in out\\s101\\scene_io\\ of --root.
 1. Round trip: the viewer writes arena + one placed actor with `record`; rsc_dump --rewrite reads it back and writes it again: identical bytes.
    The file must hold the arena, the actor (stable id "actor:..") and the camera.
 2. Planted faults: a truncated file, a wrong version and a wrong element_size each fail loudly (rsc_dump exit 2 with the reason).
 3. Draw from a file: the rdr backend's shot of arena 5 vs rsc_draw of the arena's record, with lane 6's tile comparison; the planted twin
    (textures off in rsc_draw) must fail the same comparison.
A check that cannot read its input fails loudly (never a pass)."""
import argparse
import importlib.util
import json
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MAIN = os.environ.get('RDR_MAIN_CHECKOUT') or os.path.normpath(os.path.join(HERE, '..', '..'))


def load_lane6():
    spec = importlib.util.spec_from_file_location('lane6_selftest', os.path.join(HERE, 'selftest.py'))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def run_viewer(exe, backend, game, out, name, cmds, env_extra=None):
    script = os.path.join(out, name + '.jsonl')
    with open(script, 'w') as f:
        for c in cmds:
            f.write(json.dumps(c) + '\n')
    env = dict(os.environ, **(env_extra or {}))
    with open(os.path.join(out, name + '.out.txt'), 'w') as o, open(os.path.join(out, name + '.err.txt'), 'w') as e:
        p = subprocess.run([exe, '--backend', backend, '--game', game, '--script', script, '--exit', '--hidden'], stdout=o, stderr=e, env=env, timeout=600)
    with open(os.path.join(out, name + '.out.txt'), errors='replace') as o:
        return p.returncode, o.read()


def run(cmd, env_extra=None, timeout=300):
    env = dict(os.environ, **(env_extra or {}))
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=env)
    return p.returncode, p.stdout + p.stderr


def read_bytes(path):
    with open(path, 'rb') as f:
        return f.read()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.normpath(os.path.join(HERE, '..', '..')))
    ap.add_argument('--arena', type=int, default=5)
    ap.add_argument('--wrestler', type=int, default=107)
    ap.add_argument('--size', default='640x360')
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    tools = os.path.join(root, 'out', 'tools')
    viewer, dump, draw = (os.path.join(tools, n) for n in ('wwe2k14_assets_rdr.exe', 'rsc_dump.exe', 'rsc_draw.exe'))
    game = os.path.join(MAIN, 'game_data', 'extracted')
    for need in (viewer, dump, draw, game):
        if not os.path.exists(need):
            print(f'FAIL  cannot read its input: {need}')
            return 1
    out = os.path.join(root, 'out', 's101', 'scene_io')
    os.makedirs(out, exist_ok=True)
    w, h = (int(x) for x in args.size.split('x'))
    fails = []
    lane6 = load_lane6()
    look = {'cmd': 'look', 'mode': 'lit', 'shadows': False, 'dof': False, 'fxaa': False, 'bloom': False, 'glare': False, 'gamma': False, 'haze': False}

    # ---- 1. the viewer writes arena + one actor ----
    a_rsc = os.path.join(out, 'arena_actor.rsc')
    if os.path.exists(a_rsc):
        os.remove(a_rsc)
    code, text = run_viewer(viewer, 'vk', game, out, 'record_actor', [
        look, {'cmd': 'load', 'kind': 'arena', 'id': args.arena},
        {'cmd': 'place', 'wrestler': args.wrestler, 'attire': 0, 'spot': 'ring', 'id': 'a1'},
        {'cmd': 'record', 'path': a_rsc, 'preset': 'ring', 'w': w, 'h': h}, {'cmd': 'quit'}])
    if not os.path.exists(a_rsc):
        print(f'FAIL  the viewer wrote no record (exit {code}); see {out}\\record_actor.out.txt')
        return 1
    rc, dump_text = run([dump, a_rsc, '--rewrite', os.path.join(out, 'arena_actor.again.rsc')])
    if rc != 0:
        print('FAIL  rsc_dump cannot read the viewer record: ' + dump_text.strip()[-300:])
        return 1
    ok_content = f'"arena:{args.arena}"' in dump_text and '"actor:a1"' in dump_text and 'camera: eye' in dump_text
    n_obj = dump_text.count('\n  object ')
    print(('PASS' if ok_content else 'FAIL') + f'  record content: {n_obj} objects, arena + actor "a1" + camera present={ok_content}')
    if not ok_content:
        fails.append('content')
    first, again = read_bytes(a_rsc), read_bytes(os.path.join(out, 'arena_actor.again.rsc'))
    same = first == again
    print(('PASS' if same else 'FAIL') + f'  round trip: write -> read -> write {len(first)} bytes, identical={same}')
    if not same:
        fails.append('round trip')
    skinned = 'flags 0x5' in dump_text and ' bones 0+158' in dump_text.replace('bones 0+0', '') or any(
        l.startswith('  object') and 'actor:a1' in l and 'flags 0x5' in l and 'bones 0+0' not in l for l in dump_text.splitlines())
    print(('PASS' if skinned else 'FAIL') + '  the placed actor carries its skin palette (kObjSkinned + a bone range)')
    if not skinned:
        fails.append('actor palette')

    # ---- 2. planted faults ----
    def bad_file(name, data, want):
        p = os.path.join(out, name)
        with open(p, 'wb') as f:
            f.write(data)
        rc2, txt = run([dump, p])
        loud = rc2 == 2 and want in txt
        print(('PASS' if loud else 'FAIL') + f'  planted fault {name}: exit {rc2}, wanted "{want}" in: {txt.strip()[-140:]}')
        if not loud:
            fails.append(name)

    bad_file('fault_truncated.rsc', first[:len(first) // 2], 'truncated')
    bad_file('fault_empty.rsc', b'', 'truncated')
    ver = bytearray(first)
    ver[4:8] = struct.pack('<I', 2)
    bad_file('fault_version.rsc', bytes(ver), 'version')
    # wrong element_size: the Object section's directory entry (id 4)
    count = struct.unpack_from('<I', first, 8)[0]
    es = bytearray(first)
    patched = False
    for i in range(count):
        o = 40 + i * 24
        sid, esz = struct.unpack_from('<II', first, o)
        if sid == 4:
            struct.pack_into('<I', es, o + 4, esz + 8)
            patched = True
    if not patched:
        print('FAIL  the record has no Object section to plant an element_size fault in')
        fails.append('no objects section')
    else:
        bad_file('fault_element_size.rsc', bytes(es), 'element_size')
    magic = bytearray(first)
    magic[0] ^= 0xFF
    bad_file('fault_magic.rsc', bytes(magic), 'not a scene record')

    # ---- 3. draw from a file vs the rdr backend's own shot ----
    b_rsc, shot, drawn, drawn_bad = (os.path.join(out, n) for n in ('arena.rsc', 'rdr_shot.png', 'from_file.png', 'from_file_notex.png'))
    for p in (b_rsc, shot, drawn, drawn_bad):
        if os.path.exists(p):
            os.remove(p)
    code, text = run_viewer(viewer, 'rdr', game, out, 'record_arena', [
        look, {'cmd': 'load', 'kind': 'arena', 'id': args.arena},
        {'cmd': 'record', 'path': b_rsc, 'preset': 'ring', 'w': w, 'h': h},
        {'cmd': 'shot', 'path': shot, 'preset': 'ring', 'w': w, 'h': h}, {'cmd': 'quit'}], {'WWE_RDR_VALIDATION': '1'})
    if not (os.path.exists(b_rsc) and os.path.exists(shot)):
        print(f'FAIL  the rdr session wrote no record / shot (exit {code}); see {out}\\record_arena.out.txt')
        return 1
    rc, txt = run([draw, b_rsc, '--game', game, '--out', drawn, '--size', args.size], {'WWE_RDR_VALIDATION': '1'}, 600)
    if rc != 0 or not os.path.exists(drawn):
        print('FAIL  rsc_draw: ' + txt.strip()[-400:])
        return 1
    import re
    errs = [int(m) for m in re.findall(r'validation errors: (\d+)', txt)]
    print(('PASS' if errs and max(errs) == 0 else 'FAIL') + f'  rsc_draw validation errors: {errs}')
    if not errs or max(errs) != 0:
        fails.append('validation')
    ref, got = lane6.read_png(shot), lane6.read_png(drawn)
    ok, notes = lane6.compare(ref, got)
    print(('PASS' if ok else 'FAIL') + '  picture from the file vs the rdr shot: ' + '; '.join(notes))
    if not ok:
        fails.append('picture differs')
    rc, txt = run([draw, b_rsc, '--game', game, '--out', drawn_bad, '--size', args.size], {'WWE_RDR_NO_TEXTURES': '1'}, 600)
    if rc != 0 or not os.path.exists(drawn_bad):
        print('FAIL  rsc_draw (planted twin) did not run: ' + txt.strip()[-300:])
        fails.append('twin did not run')
    else:
        tok, tnotes = lane6.compare(ref, lane6.read_png(drawn_bad))
        print(('PASS' if not tok else 'FAIL') + '  planted fault (textures off in rsc_draw) is caught: ' + '; '.join(tnotes))
        if tok:
            fails.append('twin passed')
    # rsc_draw must also refuse a damaged record
    rc, txt = run([draw, os.path.join(out, 'fault_truncated.rsc'), '--game', game, '--out', os.path.join(out, 'never.png')])
    refused = rc == 2 and not os.path.exists(os.path.join(out, 'never.png'))
    print(('PASS' if refused else 'FAIL') + f'  rsc_draw refuses a truncated record (exit {rc})')
    if not refused:
        fails.append('draw accepted a damaged record')

    if fails:
        print('FAIL  ' + '; '.join(fails))
        return 1
    print('PASS  lane 14 scene_io selftest')
    return 0


if __name__ == '__main__':
    sys.exit(main())
