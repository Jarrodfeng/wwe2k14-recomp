#!/usr/bin/env python3
"""Session 101 lane 18 (UI draw names): the offline acceptance test, registered in tools\\gate\\lanes\\ui_names.json. 0 game runs.

  py -I tools\\names\\ui_names_selftest.py [--root DIR]

 1 scope audit      every function that calls the op-19 recorder sub_82C1B350 (q.py callers) is scoped in src\\draw_origin.h (S101_UI_SCOPE) or listed
                    with its reason in tools\\names\\ui_scope_leftout.json; no listed function is stale or also scoped; the two non-recorders stay non-callers
 2 planted twin     the same audit with one scope removed (sub_82BC59D0) and with one extra caller must FAIL (a positive control)
 2b real path     names_draw_test.cpp runs the real TakeDrawOriginInto with two widgets in one frame (+ planted twin of the stack-buffer bug)
 2c U1 glyphs     the same exe: a glyph quad inside a text draw is class text and carries its whole run (UTF-8, icon codes, cap, truncation);
                    planted twin: names_draw.h built without the promotion line (-DS101_PLANT_NO_TEXT_PROMOTION), the same checks must fail
 3 old capture      s100cards (made before the names existed) opens in fview (validation 1, 0 errors) and the top draw's origin reads "unnamed"
 4 widget round trip a synthetic frame (s100cards frame 1 + a DrawOrigin section and a string table written in adapter_frame.h's layout) opens in fview
                    with 0 validation errors, in rplay too, and the top draw's origin shows its widget string
 5 planted twin     the same frame with widget = none must NOT show the widget (the check discriminates)
Needs the main checkout's out\\captures\\s100cards, generated\\ (q.py), out\\renderer\\dev\\rplay.exe, and fview.exe (tools\\fview\\build.ps1) in --root.
"""
import argparse, json, os, re, shutil, struct, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
results = []


def report(name, ok, evidence):
    results.append(ok)
    print(f'{"PASS" if ok else "FAIL"}  {name}: {evidence}'.encode('ascii', 'backslashreplace').decode('ascii'), flush=True)  # (S101 U1: a run string holds private-use icon codes; a cp1252 console cannot print them)


def run(cmd, timeout=900, cwd=None):
    p = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, timeout=timeout, cwd=cwd)
    return p.returncode, p.stdout + p.stderr


def audit(callers, scoped, leftout, not_recorders):
    """Problems found; an empty list = the audit passes."""
    probs = []
    for c in sorted(callers):
        if c not in scoped and c not in leftout:
            probs.append(f'caller 0x{c:08X} is neither scoped nor listed with a reason')
    for l in sorted(leftout):
        if l not in callers:
            probs.append(f'listed 0x{l:08X} is not an op-19 caller any more (stale)')
        if l in scoped:
            probs.append(f'0x{l:08X} is both scoped and listed')
    for n in sorted(not_recorders):
        if n in callers:
            probs.append(f'0x{n:08X} is listed as a non-recorder but q says it calls the recorder')
    for s in sorted(scoped):
        if s not in callers:
            probs.append(f'scoped 0x{s:08X} is not an op-19 caller (a scope on a function that records nothing)')
    return probs


def find_main(root):
    if (root / 'out' / 'captures').exists():
        return root
    return root.parents[2]  # a worktree: <main>\.claude\worktrees\NAME


def build_capture(src, dst, frame, origins, strings, runs=()):
    """dst = src with frames/frame_<frame>.bin rewritten to carry sections 64 (DrawOrigin[draws]) and 67 (strings), and 69 (TextRun[]: draw, string, units, flags) when `runs` is given. Other files are hard links."""
    for p in src.rglob('*'):
        rel = p.relative_to(src)
        t = dst / rel
        if p.is_dir():
            t.mkdir(parents=True, exist_ok=True)
        elif rel.parts[0] == 'frames' and p.name == f'frame_{frame:06d}.bin':
            continue
        else:
            t.parent.mkdir(parents=True, exist_ok=True)
            try:
                os.link(p, t)
            except OSError:
                shutil.copyfile(p, t)
    data = (src / 'frames' / f'frame_{frame:06d}.bin').read_bytes()
    magic, version, cf, sc = struct.unpack_from('<IIII', data, 0)
    assert magic == 0x52464352, 'not a frame file'
    entries = []
    for s in range(sc):
        sid, esize, count, off = struct.unpack_from('<IIQQ', data, 32 + 24 * s)
        entries.append([sid, esize, count, off])
    assert not any(e[0] in (64, 67, 69) for e in entries), 'the source frame already has names'
    draws = next(e for e in entries if e[0] == 4)[2]
    blobs = [(e[0], e[1], e[2], data[e[3]:e[3] + e[1] * e[2]]) for e in entries]
    org = bytearray()
    for d in range(draws):
        o = origins.get(d)
        org += struct.pack('<BBHIIIIIII', *(o if o else (0, 0, 0, 0, 0, 0, 0, 0, 0, 0)))
    blobs.append((64, 32, draws, bytes(org)))
    blobs.append((67, 1, len(strings), bytes(strings)))
    if runs:
        blobs.append((69, 16, len(runs), b''.join(struct.pack('<IIII', *r) for r in runs)))
    hdr = bytearray(data[:32 + 24 * sc][:32])
    struct.pack_into('<I', hdr, 12, len(blobs))  # FrameFileHeader::section_count is the 4th u32 (the selftest of fview reads it the same way)
    offset = 32 + 24 * len(blobs)
    table, body = bytearray(), bytearray()
    for sid, esize, count, blob in blobs:
        table += struct.pack('<IIQQ', sid, esize, count, offset + len(body))
        body += blob
    (dst / 'frames' / f'frame_{frame:06d}.bin').write_bytes(bytes(hdr) + bytes(table) + bytes(body))
    return draws


def top_draw(fview, root, cap, cfg, extra=()):
    j = Path(tempfile.mkdtemp(prefix='l18_')) / 'p.json'
    code, out = run([fview, '--root', root, '--validation', '1', '--capture', cap, '--frame', cfg['frame'], '--pixel', f'{cfg["x"]},{cfg["y"]}',
                     '--surface', cfg['surface'], '--through-op', cfg['through_op'], '--json', j, *extra], timeout=1800)
    try:
        h = json.loads(j.read_text(encoding='utf-8'))
        return code, out, h['entries'][h['top_wrote_index']]['draw']
    except Exception as e:
        return code, out, {'error': repr(e)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=str(HERE.parent.parent))
    args = ap.parse_args()
    root = Path(args.root).resolve()
    main_co = find_main(root)

    # 1 + 2: the scope audit and its planted twins
    code, out = run([sys.executable, '-I', root / 'tools' / 'q' / 'q.py', 'callers', '82C1B350'], cwd=main_co)
    callers = {int(m.group(1), 16) for m in re.finditer(r'^\s+sub_([0-9A-F]{8})\s', out, re.M)}
    header = re.search(r'(\d+) call sites in (\d+) functions', out)
    src = (root / 'src' / 'draw_origin.h').read_text(encoding='utf-8', errors='replace')
    scoped = {int(m, 16) for m in re.findall(r'S101_UI_(?:TEXT_)?SCOPE\(sub_([0-9A-F]{8})\s*,', src)}
    lj = json.loads((HERE / 'ui_scope_leftout.json').read_text(encoding='utf-8'))
    leftout = {int(k, 16) for k in lj['leftout']}
    nonrec = {int(k, 16) for k in lj['not_recorders']}
    ok_read = code == 0 and header and len(callers) == int(header.group(2)) and len(callers) > 5 and len(scoped) > 3
    report('scope audit: the q caller list was read', ok_read, f'{len(callers)} callers parsed (q says {header.group(2) if header else "?"} functions), {len(scoped)} scoped, {len(leftout)} listed')
    probs = audit(callers, scoped, leftout, nonrec) if ok_read else ['could not read the caller list']
    report('scope audit: every op-19 caller is scoped or listed with a reason', not probs, '; '.join(probs) if probs else f'{len(callers)} callers = {len(scoped)} scoped + {len(leftout)} listed')
    if ok_read:
        twin1 = audit(callers, scoped - {0x82BC59D0}, leftout, nonrec)
        report('planted twin: one scope removed (sub_82BC59D0) is caught', any('82BC59D0' in p for p in twin1), '; '.join(twin1) or 'NOT CAUGHT')
        twin2 = audit(callers | {0x82BC0001}, scoped, leftout, nonrec)
        report('planted twin: an unlisted extra caller is caught', any('82BC0001' in p for p in twin2), '; '.join(twin2) or 'NOT CAUGHT')
        twin3 = audit(callers - {0x827568E0}, scoped, leftout, nonrec)
        report('planted twin: a stale listed entry is caught', any('827568E0' in p for p in twin3), '; '.join(twin3) or 'NOT CAUGHT')

    # 2b: the REAL draw-origin step (names_draw.h) with two widgets in one frame, and its planted twin (review 1)
    clang = Path('C:/Program Files/LLVM/bin/clang++.exe')
    exe_t = root / 'out' / 'names' / 'names_draw_test.exe'
    exe_t.parent.mkdir(parents=True, exist_ok=True)
    code, out = run([clang, '-std=c++20', '-Wall', '-Wextra', '-I', root / 'src' / 'renderer_adapter', HERE / 'names_draw_test.cpp', '-o', exe_t], timeout=300) if clang.exists() else (1, 'clang++ not found')
    report('names_draw_test builds', code == 0, out.strip()[-200:] or 'ok')
    if code == 0:
        code, out = run([exe_t])
        report('real draw-origin path: two widgets in one frame keep their own strings; registry rejects recycled elements', code == 0 and 'all pass' in out and 'FAIL' not in out,
               next((l for l in out.splitlines() if l.startswith('FAIL')), 'all pass'))
        code, out = run([exe_t, '--planted'])
        report('planted twin: the old stack-buffer call shape makes two widgets share a string', code == 0 and 'same offsets' in out, out.strip().splitlines()[-1] if out.strip() else '(no output)')
        # S101 U1: glyph quads are class text and carry their whole run (the checks are in the run above). Planted: names_draw.h compiled WITHOUT the
        # sprite -> text promotion line; the same checks must fail.
        exe_p = root / 'out' / 'names' / 'names_draw_test_plant.exe'
        code, out = run([clang, '-std=c++20', '-DS101_PLANT_NO_TEXT_PROMOTION', '-I', root / 'src' / 'renderer_adapter', HERE / 'names_draw_test.cpp', '-o', exe_p], timeout=300)
        if code == 0:
            code, out = run([exe_p])
        fails = [l for l in out.splitlines() if l.startswith('FAIL') and 'U1' in l]
        report('planted twin (U1): without the text promotion the glyph checks fail', code != 0 and len(fails) >= 3, f'exit {code}; {len(fails)} U1 checks failed, e.g. {fails[0] if fails else out.strip()[-150:]}')

    # 3-5: captures
    sys.path.insert(0, str(root / 'tools' / 'fview'))   # lane 218: the shared locator (refuses an exe older than the renderer sources)
    import fview_exe as fe
    try:
        fview = Path(fe.find_fview(root=root, kind='dev'))
    except fe.FviewError as e:
        report('captures', False, str(e))
        return 1
    rplay = next((p for p in (root / 'out' / 'renderer' / 'dev' / 'rplay.exe', main_co / 'out' / 'renderer' / 'dev' / 'rplay.exe') if p.exists()), None)
    cap = main_co / 'out' / 'captures' / 's100cards'
    cfg = json.loads((root / 'tools' / 'fview' / 'selftest_cards.json').read_text())
    if not fview.exists() or not cap.exists():
        report('captures', False, f'needs {fview} (tools\\fview\\build.ps1) and {cap}')
        return 1
    code, out, d = top_draw(fview, root, cap, cfg)
    org = d.get('origin', '')
    report('old capture (no names) opens in fview; the draw reads unnamed', code == 0 and 'validation errors 0' in out and 'unnamed (this frame carries no names)' in org,
           f'exit {code}; origin: {org or d}')
    tmp = Path(tempfile.mkdtemp(prefix='l18_syn_'))
    widget = 'screen seq=75 menu=2; element 0x83A00010 node 0x83B00020 text 45004'
    ok_d = isinstance(d.get('draw'), int)
    if not ok_d:
        report('synthetic frame', False, f'no top draw index from the old capture: {d}')
        return 1
    strings = b'T_Sprite\0' + widget.encode() + b'\0'
    tech_off, widget_off = 1, 1 + len(b'T_Sprite\0')
    # DrawOrigin: klass(3 = sprite) op(19) flags(1 = from submit) call_site pass_site object part technique asset widget
    named = {d['draw']: (3, 19, 1, 0x82BC4A00, 0x82BC8064, 0x83A00010, 0x1234, tech_off, 0, widget_off)}
    syn = tmp / 'named'
    build_capture(cap, syn, cfg['frame'], named, strings)
    code, out, d2 = top_draw(fview, root, syn, cfg)
    o2 = d2.get('origin', '')
    report('widget round trip: fview shows the widget of a synthetic named frame', code == 0 and 'validation errors 0' in out and widget in o2 and 'sprite T_Sprite' in o2,
           f'exit {code}; origin: {o2 or d2}')
    if rplay:
        code, out = run([rplay, '--capture', syn, '--validation', '1', '--out', tmp / 'syn.png'], timeout=1800)
        report('widget round trip: rplay opens the synthetic frame (it shows no names: validation only)', code == 0 and (tmp / 'syn.png').exists(), f'exit {code}')
    else:
        report('widget round trip: rplay', False, 'rplay.exe not found (tools\\renderer\\build.ps1)')
    # S101 U1: the same draw as a glyph (class text = 4) with its whole run string, section 69 (an icon code and an accent survive as UTF-8)
    run_text = 'PRESS \u00e9\ue00c TO CONTINUE, THEN MORE THAN FORTY CHARACTERS OF LABEL'
    rstr = strings + run_text.encode('utf-8') + b'\0'
    run_off = len(strings) + 1
    glyph = {d['draw']: (4, 19, 1, 0x82BC3600, 0x82BD0600, 0x83A00010, 0x1234, 0, 0, widget_off)}
    gl = tmp / 'glyph_run'
    build_capture(cap, gl, cfg['frame'], glyph, rstr, runs=[(d['draw'], run_off, len(run_text), 0)])
    code, out, d4 = top_draw(fview, root, gl, cfg)
    o4 = d4.get('origin', '')
    report('glyph round trip (U1): fview shows class text and the whole run string from section 69', code == 0 and 'validation errors 0' in out and o4.startswith('text') and f'run "{run_text}"' in o4 and f'({len(run_text)} units)' in o4,
           f'exit {code}; origin: {o4 or d4}')
    nr = tmp / 'glyph_norun'
    build_capture(cap, nr, cfg['frame'], glyph, strings)
    code, out, d5 = top_draw(fview, root, nr, cfg)
    o5 = d5.get('origin', '')
    report('planted twin (U1): a glyph draw frame without section 69 shows no run string', code == 0 and o5.startswith('text') and ', run "' not in o5, f'origin: {o5 or d5}')
    twin = tmp / 'unnamed_widget'
    named_none = {d['draw']: (3, 19, 1, 0x82BC4A00, 0x82BC8064, 0x83A00010, 0x1234, tech_off, 0, 0)}
    build_capture(cap, twin, cfg['frame'], named_none, strings)
    code, out, d3 = top_draw(fview, root, twin, cfg)
    o3 = d3.get('origin', '')
    report('planted twin: a 2D draw without a widget reads "widget unnamed" and never shows the string', code == 0 and widget not in o3 and 'widget unnamed' in o3, f'origin: {o3 or d3}')
    shutil.rmtree(tmp, ignore_errors=True)
    bad = results.count(False)
    print(f'{len(results) - bad} of {len(results)} checks pass')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
