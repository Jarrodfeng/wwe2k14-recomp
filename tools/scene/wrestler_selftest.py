#!/usr/bin/env python3
"""Lane 29 acceptance test (class C4, wrestlers; offline, 0 game runs).

  py -I tools\\scene\\wrestler_selftest.py [--root DIR] [--size 640x360]

Needs out\\tools\\wrestler_emit.exe (tools\\scene\\build_wrestler.ps1) under --root, and the Arena Viewer (wwe2k14_assets_rdr.exe) + rsc_draw.exe
under --root\\out\\tools or, failing that, under the main checkout's out\\tools (built by the scene / scene_io gate checks).
 1. wrestler_emit selftest: the contributor's unit checks (last update wins, skin = inv_bind x world, ids, counters) with their planted twins.
 2. Per wrestler (the same three as lane 13: cloth, gear, transparency): the viewer writes a record of the posed wrestler (arena 5, idle frame 30);
    wrestler_emit feeds its palette through the contributor as three updates in one frame (bind pose, a pose off by 3 units, the real pose LAST)
    and writes the record back with the contributor's palette; rsc_draw draws that file; the wrestler's region is compared with the viewer's own
    vk shot with lane 6's tile bar (selftest.compare_crop). Validation errors must be 0.
 3. Planted faults, each of which must FAIL the same bar: --fault bind (only the bind pose reaches the contributor) and (lane 43) --fault exit
    (no read at the consumer: the exit palette of sub_82BD30E8 stands; the real pose arrives only through SubmitConsumer in the passing arm).
    (The 'first update wins' fault is caught by step 1's exact comparison; at picture level it is a one-frame pose difference, below the bar.)
A check that cannot read its input fails loudly (never a pass)."""
import argparse
import importlib.util
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MAIN = os.environ.get('RDR_MAIN_CHECKOUT') or os.path.normpath(os.path.join(HERE, '..', '..'))


def load_lane6():
    spec = importlib.util.spec_from_file_location('lane6_selftest', os.path.join(HERE, 'selftest.py'))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def run(cmd, env_extra=None, timeout=600):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=dict(os.environ, **(env_extra or {})))
    return p.returncode, p.stdout + p.stderr


def run_draw(cmd, env_extra=None):
    """rsc_draw, retried when the shared GPU is momentarily full of other lanes' viewers ('Not enough graphics memory': a start-up refusal, not a
    drawing result). Any other failure is returned at once."""
    import time
    for attempt in range(6):
        rc, text = run(cmd, env_extra)
        if rc != 3 or 'Not enough graphics memory' not in text:
            return rc, text
        time.sleep(5 * (attempt + 1))
    return rc, text


def find_tool(root, name):
    for base in (root, MAIN):
        p = os.path.join(base, 'out', 'tools', name)
        if os.path.exists(p):
            return p
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.normpath(os.path.join(HERE, '..', '..')))
    ap.add_argument('--size', default='640x360')
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    emit = os.path.join(root, 'out', 'tools', 'wrestler_emit.exe')
    viewer, draw = find_tool(root, 'wwe2k14_assets_rdr.exe'), find_tool(root, 'rsc_draw.exe')
    game = os.path.join(MAIN, 'game_data', 'extracted')
    for need in (emit, viewer, draw, game):
        if not need or not os.path.exists(need):
            print(f'FAIL  cannot read its input: {need}')
            return 1
    try:
        import numpy as np
        from PIL import Image
    except ImportError as e:
        print(f'FAIL  needs numpy and Pillow ({e})')
        return 1
    lane6 = load_lane6()
    out = os.path.join(root, 'out', 's101', 'c4_wrestlers')
    os.makedirs(out, exist_ok=True)
    w, h = (int(x) for x in args.size.split('x'))
    fails = []

    rc, text = run([emit, 'selftest'])
    print(text.rstrip())
    if rc != 0:
        fails.append('contributor selftest')

    for wid, attire, kind, _expect in lane6.WRESTLERS:
        tag = f'w{wid}_{attire}'
        base = lane6.wrestler_cmds(wid, attire, (w, h))
        # the viewer's own picture (vk): the reference
        shot, log, code = lane6.run_scene(viewer, 'vk', game, out, f'{tag}_vk', {}, base)
        if not os.path.exists(shot):
            print(f'FAIL  {tag}: no vk shot (exit {code}); see {log}')
            fails.append(f'{tag}: no vk shot')
            continue
        rec = os.path.join(out, f'{tag}.rsc')
        if os.path.exists(rec):
            os.remove(rec)
        cmds = [c for c in base if c.get('cmd') not in ('shot', 'quit')] + [
            {'cmd': 'record', 'path': rec.replace('\\', '/'), 'w': w, 'h': h}, {'cmd': 'quit'}]
        lane6.run_scene(viewer, 'vk', game, out, f'{tag}_rec', {}, cmds)
        if not os.path.exists(rec):
            print(f'FAIL  {tag}: the viewer wrote no record; see {out}\\{tag}_rec.err.txt')
            fails.append(f'{tag}: no record')
            continue
        ref = np.asarray(Image.open(shot).convert('RGB'))
        results = {}
        for label, extra in (('base', None), ('contrib', []), ('bind', ['--fault', 'bind']), ('exit', ['--fault', 'exit'])):
            src = rec
            if extra is not None:
                src = os.path.join(out, f'{tag}_{label}.rsc')
                if os.path.exists(src):
                    os.remove(src)
                rc, text = run([emit, 'emit', rec, src] + extra)
                if rc != 0 or not os.path.exists(src):
                    print(f'FAIL  {tag} {label}: wrestler_emit: {text.strip()[-300:]}')
                    fails.append(f'{tag} {label}: emit')
                    continue
            if label == 'contrib':
                # Emit's own record, unpatched (placeholder asset, source Live): rsc_draw must accept it and skip the unknown-asset objects.
                live, lpng = src + '.live.rsc', os.path.join(out, f'{tag}_live.png')
                if os.path.exists(lpng):
                    os.remove(lpng)
                rc, text = run_draw([draw, live, '--game', game, '--out', lpng, '--size', args.size], {'WWE_RDR_VALIDATION': '1'})
                good = rc == 0 and os.path.exists(lpng) and 'objects drawn' in text and 'asset unknown' in text
                print(('PASS' if good else 'FAIL') + f'  {tag} Emit\'s own live record is drawn by rsc_draw (unknown-asset objects skipped): exit {rc}')
                if not good:
                    fails.append(f'{tag}: live record')
            png = os.path.join(out, f'{tag}_{label}.png')
            if os.path.exists(png):
                os.remove(png)
            rc, text = run_draw([draw, src, '--game', game, '--out', png, '--size', args.size], {'WWE_RDR_VALIDATION': '1'})
            if rc != 0 or not os.path.exists(png):
                print(f'FAIL  {tag} {label}: rsc_draw: {text.strip()[-300:]}')
                fails.append(f'{tag} {label}: rsc_draw')
                continue
            errs = [int(m) for m in re.findall(r'validation errors(?: so far)?: (\d+)', text)]
            ok, notes = lane6.compare_crop(np, ref, np.asarray(Image.open(png).convert('RGB')))
            results[label] = (ok, notes, errs)
        for label in ('base', 'contrib', 'bind', 'exit'):
            if label not in results:
                continue
            ok, notes, errs = results[label]
            want = label not in ('bind', 'exit')
            good = ok == want
            what = {'base': 'the viewer record drawn from the file (baseline)', 'contrib': 'palette through the contributor',
                    'bind': 'planted fault (bind pose only) is caught',
                    'exit': 'planted fault (lane 43: the exit palette of sub_82BD30E8, no read at the consumer) is caught'}[label]
            print(('PASS' if good else 'FAIL') + f'  {tag} ({kind}) {what}: ' + '; '.join(notes))
            if not good:
                fails.append(f'{tag} {label}')
            if label not in ('bind', 'exit'):
                vok = bool(errs) and max(errs) == 0
                print(('PASS' if vok else 'FAIL') + f'  {tag} {label} rsc_draw validation errors: {errs if errs else "no validation line"}')
                if not vok:
                    fails.append(f'{tag} {label}: validation')
    if fails:
        print('FAIL  ' + '; '.join(fails))
        return 1
    print('PASS  lane 29 c4_wrestlers selftest')
    return 0


if __name__ == '__main__':
    sys.exit(main())
