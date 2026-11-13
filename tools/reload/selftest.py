#!/usr/bin/env python3
"""Session 101 lane 24: the live shader reload's offline acceptance test (gate lane file tools\\gate\\lanes\\reload.json). 0 game runs: it starts the Arena Viewer
(rdr backend, --hidden) and fview headless; neither is the game.

  py -I tools\\reload\\selftest.py [--root DIR] [--only viewer|fview]

Needs out\\tools\\wwe2k14_assets_rdr.exe (tools\\assets\\build_viewer.ps1 -Backend rdr), out\\renderer\\dev\\fview.exe (tools\\fview\\build.ps1), the capture s100cards
and game_data\\extracted in the MAIN checkout. Every check prints PASS / FAIL with its evidence; an input it cannot read is a FAIL. Exit 1 on any FAIL.

 viewer  one process, --shader_dir + --shader_src (a temp copy of the effect sources):
   1 shot A of arena 5 ("ring").
   2 edit one bound arena effect in the copy (its colour multiplied by (0.25, 1, 1)); wait for "[reload] E: n variant(s) rebuilt"; shot B. B differs from A, ONLY in
     the red channel and only downwards (every other effect's pixels are untouched, and the changed ones are that effect's: green and blue never move).
   3 revert the edit; wait; shot C == A byte for byte.
   4 a syntax error: the log says "[reload] FAILED E_...: <effect>.frag:<line>: ..." (the source file and line); shot D == C byte for byte (the old pipeline stays).
   5 the time from the edit to the new picture, stated.
 fview   capture s100cards, the scene colour after op 2932:
   1 A = the picture. 2 B = --apply-reload yAudience from an edited copy (colour multiply): the pixels that moved are exactly yAudience's (fview's own pixel history
     names the top draw: every sampled changed pixel -> yAudience, every sampled bright unchanged pixel -> another effect). 3 --apply-reload from the pristine copy == A.
   4 a syntax error with --reload-continue: the failure names file:line, the picture == A. 5 --shader_dir <folder the compiler wrote from the edit> == B.
   6 planted fault: the same folder with one layout json changed -> refused, picture == A.
"""
import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
results = []


def report(name, ok, evidence):
    results.append((name, ok))
    print(f'{"PASS" if ok else "FAIL"}  {name}: {evidence}', flush=True)


def read_png(path):
    """Minimal PNG reader (8-bit RGB / RGBA, no interlace): (w, h, channels, rows)."""
    data = Path(path).read_bytes()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise RuntimeError(f'{path}: not a PNG')
    pos, idat, ctype, w, h = 8, b'', 0, 0, 0
    while pos < len(data):
        n, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if kind == b'IHDR':
            w, h, depth, ctype, _, _, interlace = struct.unpack('>IIBBBBB', body)
            if depth != 8 or ctype not in (2, 6) or interlace:
                raise RuntimeError(f'{path}: unsupported PNG')
        elif kind == b'IDAT':
            idat += body
    ch = 4 if ctype == 6 else 3
    raw = zlib.decompress(idat)
    stride = w * ch
    rows, prev, p = [], bytearray(stride), 0
    for _ in range(h):
        ft = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        for i in range(stride):
            a = line[i - ch] if i >= ch else 0
            b = prev[i]
            c = prev[i - ch] if i >= ch else 0
            if ft == 1: line[i] = (line[i] + a) & 255
            elif ft == 2: line[i] = (line[i] + b) & 255
            elif ft == 3: line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif ft == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else (b if pb <= pc else c))) & 255
        rows.append(line)
        prev = line
    return w, h, ch, rows


def compare(a, b):
    """(equal, changed pixel count, red_only_down, [(x, y)] changed, [(x, y, red)] unchanged with red > 8). a, b = read_png results."""
    if a[:3] != b[:3]:
        return False, -1, False, [], []
    w, h, ch, ra = a
    rb = b[3]
    changed, same_bright, ok_channels = [], [], True
    for y in range(h):
        if ra[y] == rb[y]:
            for x in range(0, w, 7):
                if ra[y][x * ch] > 8:
                    same_bright.append((x, y, ra[y][x * ch]))
            continue
        for x in range(w):
            o = x * ch
            if ra[y][o:o + ch] != rb[y][o:o + ch]:
                changed.append((x, y))
                if ra[y][o + 1] != rb[y][o + 1] or ra[y][o + 2] != rb[y][o + 2] or rb[y][o] > ra[y][o]:
                    ok_channels = False
            elif ra[y][o] > 8 and x % 7 == 0:
                same_bright.append((x, y, ra[y][o]))
    return not changed, len(changed), ok_channels, changed, same_bright


def multiply_edit(path, guard=None):
    """Colour x (0.25, 1, 1) at the end of the fragment shader: the original main() is renamed and a new one calls it. `guard`: a preprocessor condition; the
    multiply is then only in the variants that define it (the variants of one effect share the file, and some share a game shader with other effects)."""
    text = Path(path).read_text(encoding='utf-8')
    if 'void main()' not in text:
        raise RuntimeError(f'{path}: no main()')
    mul = 'outColor.rgb *= vec3(0.25, 1.0, 1.0);'
    body = f'\n#if {guard}\n{mul}\n#endif\n' if guard else mul
    text = text.replace('void main()', 'void fx_orig_main()', 1) + '\nvoid main() { fx_orig_main(); ' + body + ' }\n'
    Path(path).write_text(text, encoding='utf-8')


def break_edit(path):
    text = Path(path).read_text(encoding='utf-8')
    Path(path).write_text(text + '\nvoid fx_broken() { vec3 a = ; }\n', encoding='utf-8')


def shared_variants(fx_dir):
    """{variant name: [the other variants that list one of its game hashes]}: the registry holds ONE variant per game shader hash, so an edit of a variant that shares a
    hash with another effect's variant moves that effect's draws too. A plant must use a sole target (no entry here)."""
    by_hash = {}
    for jp in sorted(Path(fx_dir).glob('*.fx.json')):
        spec = json.loads(jp.read_text(encoding='utf-8'))
        for stage in ('vertex', 'fragment'):
            for v in spec.get(stage, []):
                for h in v.get('hashes', []):
                    by_hash.setdefault(h, []).append(f'{spec["effect"]}_{v["name"]}_{stage}')
    out = {}
    for names in by_hash.values():
        if len(names) > 1:
            for n in names:
                out.setdefault(n, []).extend(x for x in names if x != n)
    return out


# ---- the viewer ------------------------------------------------------------------------------------------------------------------

class Pipe:
    def __init__(self, cmd, name, log):
        self.log = log
        # The reload check judges one effect's own pixels: the rdr viewer's post pass (lane 34: haze / DoF mix neighbours and channels) is off here.
        self.proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=open(log, 'wb'), env=dict(os.environ, WWE_RDR_NO_POST='1'))
        path = r'\\.\pipe\%s' % name
        self.pipe = None
        for _ in range(300):
            if self.proc.poll() is not None:
                raise RuntimeError(f'viewer exited at start ({self.proc.returncode}); see {log}')
            try:
                self.pipe = open(path, 'r+b', buffering=0)
                break
            except OSError:
                time.sleep(0.1)
        if self.pipe is None:
            self.kill()
            raise RuntimeError('viewer pipe never appeared')

    def send(self, cmd, timeout=180.0):
        proc = self.proc
        timer = threading.Timer(timeout, lambda: proc.kill() if proc.poll() is None else None)
        timer.daemon = True
        timer.start()
        try:
            self.pipe.write((json.dumps(cmd) + '\n').encode())
            buf = b''
            while not buf.endswith(b'\n'):
                ch = self.pipe.read(1)
                if not ch:
                    raise RuntimeError(f'pipe closed (no answer to {cmd.get("cmd")} within {timeout:.0f} s)')
                buf += ch
            return json.loads(buf.decode('utf-8', 'replace'))
        finally:
            timer.cancel()

    def kill(self):
        try:
            if self.proc.poll() is None:
                self.proc.kill()
        except OSError:
            pass

    def stop(self):
        try:
            if self.pipe:
                self.pipe.write(b'{"cmd":"quit"}\n')
                self.pipe.close()
        except OSError:
            pass
        try:
            self.proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            self.kill()

    def log_text(self):
        return Path(self.log).read_text(encoding='utf-8', errors='replace')


def wait_for(pipe, pattern, since, timeout=60.0):
    """Waits until the viewer's log (after offset `since`) matches `pattern`; returns (match or None, new offset, seconds waited)."""
    start = time.time()
    rx = re.compile(pattern)
    while time.time() - start < timeout:
        text = pipe.log_text()
        m = rx.search(text, since)
        if m:
            return m, m.end(), time.time() - start
        time.sleep(0.05)
    return None, since, time.time() - start


def viewer_part(root, main, work):
    exe = root / 'out' / 'tools' / 'wwe2k14_assets_rdr.exe'
    game = main / 'game_data' / 'extracted'
    for need in (exe, game):
        if not need.exists():
            report('viewer: inputs', False, f'cannot read {need}')
            return
    fx = work / 'viewer_fx'
    shutil.copytree(root / 'src' / 'renderer' / 'shaders' / 'fx', fx)
    shaders = work / 'viewer_shaders'
    name = f'reload_{os.getpid()}'
    cmd = [str(exe), '--backend', 'rdr', '--game', str(game), '--pipe', name, '--hidden', '--size', '640x360', '--parent', str(os.getpid()),
           '--shader_dir', str(shaders), '--shader_src', str(fx), '--shader_root', str(root)]
    pipe = Pipe(cmd, name, str(work / 'viewer.err.txt'))
    try:
        for c in ({'cmd': 'look', 'mode': 'lit', 'shadows': False, 'dof': False, 'fxaa': False, 'bloom': False, 'glare': False, 'gamma': False},
                  {'cmd': 'load', 'kind': 'arena', 'id': 5}, {'cmd': 'camera', 'preset': 'ring'}):
            r = pipe.send(c)
            if r.get('ok') is False:
                report('viewer: setup', False, f'{c["cmd"]}: {r}')
                return

        def shot(label):
            path = work / f'viewer_{label}.png'
            if path.exists():
                path.unlink()
            t0 = time.time()
            r = pipe.send({'cmd': 'shot', 'path': str(path).replace('\\', '/'), 'w': 640, 'h': 360})
            if not r.get('ok') or not path.exists():
                raise RuntimeError(f'shot {label} failed: {r}')
            return read_png(path), time.time() - t0

        a, _ = shot('A')
        text = pipe.log_text()
        lines = re.findall(r'^\s+(\w+)\s+bound to our effect\s+(\S+) \+ (\S+)\s+\((\d+) draws\)', text, re.M)  # (material class, vertex variant, fragment variant, draws)
        shared = shared_variants(root / 'src' / 'renderer' / 'shaders' / 'fx')
        effect, why_not = None, []
        for cls in dict.fromkeys(l[0] for l in lines if l[0].startswith('yBG_')):
            # The edit changes every fragment variant of the effect: none may share a game hash with another effect's variant (the registry has one variant per hash), and
            # every material class the coverage list shows bound to a variant of this effect must be the effect's own class (or its LM twin's: LmTwin).
            spec = json.loads((root / 'src' / 'renderer' / 'shaders' / 'fx' / f'{cls}.fx.json').read_text(encoding='utf-8'))
            frag_names = [f'{cls}_{v["name"]}_fragment' for v in spec['fragment']]
            spill = [n for n in frag_names if n in shared]
            owners = {l[0] for l in lines if any(l[2] == n for n in frag_names)}
            if spill:
                why_not.append(f'{cls}: shares a game hash ({spill[0]} with {shared[spill[0]][0]})')
            elif owners - {cls, cls[:-2] if cls.endswith('LM') else cls}:
                why_not.append(f'{cls}: also bound by {sorted(owners)}')
            else:
                effect = cls
                break
        if not effect:
            report('viewer: a sole-target arena effect to edit', False, 'none of the arena\'s yBG_* effects qualifies: ' + '; '.join(why_not) + '; log: ' + text[-300:])
            return
        report('viewer: the edited effect is a sole target (no shared game hash; only its own material class binds its variants)', True,
               f'{effect}: bound by {sorted({l[0] for l in lines if l[2].startswith(effect + "_")})}; skipped: {"; ".join(why_not) or "none"}')
        frag = fx / f'{effect}.frag'
        orig = frag.read_text(encoding='utf-8')
        mark = len(pipe.log_text())

        # 2 the edit
        multiply_edit(frag)
        t_edit = time.time()
        m, mark, waited = wait_for(pipe, rf'\[reload\] {effect}: (\d+) variant\(s\) rebuilt in (\d+) ms', mark)
        if not m:
            report('viewer: edit reloads', False, f'no "[reload] {effect}: ... rebuilt" line within {waited:.0f} s; log tail: ' + pipe.log_text()[-600:])
            return
        t_log = time.time() - t_edit
        b, shot_s = shot('B')
        t_pic = time.time() - t_edit
        equal, count, channels_ok, changed, bright_same = compare(a, b)
        report('viewer: edit -> new picture, only that effect\'s pixels (red down, green / blue untouched)', (not equal) and channels_ok and 0 < count < 640 * 360,
               f'{effect}: {m.group(1)} variant(s) rebuilt ({m.group(2)} ms in the watcher), {count} of {640 * 360} pixels changed, green/blue untouched and red only down: {channels_ok}')
        report('viewer: only the programs of the changed variants are made again', bool(re.search(r'\[reload\] (\d+) of (\d+) programs forgotten', pipe.log_text())) and
               int(re.search(r'\[reload\] (\d+) of (\d+) programs forgotten', pipe.log_text()).group(1)) < int(re.search(r'\[reload\] (\d+) of (\d+) programs forgotten', pipe.log_text()).group(2)),
               (re.search(r'\[reload\] \d+ of \d+ programs forgotten[^\n]*', pipe.log_text()) or re.search('.', 'no "programs forgotten" line')).group(0))
        report('viewer: reload time (edit -> log line / edit -> new picture)', True, f'{t_log:.2f} s to the log line, {t_pic:.2f} s to the new picture (the shot itself {shot_s:.2f} s)')

        # 3 revert
        frag.write_text(orig, encoding='utf-8')
        m, mark, waited = wait_for(pipe, rf'\[reload\] {effect}: (\d+) variant\(s\) rebuilt', mark)
        if not m:
            report('viewer: revert reloads', False, f'no rebuilt line within {waited:.0f} s')
            return
        c, _ = shot('C')
        eq_c = compare(a, c)[0]
        report('viewer: revert -> the first picture again, byte for byte', eq_c, f'shot C == shot A: {eq_c}')

        # 4 a broken edit
        break_edit(frag)
        m, mark, waited = wait_for(pipe, rf'\[reload\] FAILED {effect}_\w+: ({re.escape(effect)}\.frag):(\d+): ', mark)
        d, _ = shot('D')
        eq_d = compare(c, d)[0]
        report('viewer: a syntax error is logged with file and line, the picture stays', bool(m) and eq_d,
               (f'"[reload] FAILED ...: {m.group(1)}:{m.group(2)}" after {waited:.1f} s; ' if m else f'no FAILED line with file:line within {waited:.0f} s; ') + f'shot D == shot C: {eq_d}')
        frag.write_text(orig, encoding='utf-8')
        m, mark, waited = wait_for(pipe, rf'\[reload\] {effect}: (\d+) variant\(s\) rebuilt', mark)
        time.sleep(0.6)
        mark = len(pipe.log_text())  # (after the revert's own 'published' line)
        # a shared include (fx_bg.glsl: every yBG_* effect) is edited: ALL the effects it reaches are published in one step
        bg = fx / 'fx_bg.glsl'
        bg.write_text(bg.read_text(encoding='utf-8') + '\n// lane 24 selftest: a comment\n', encoding='utf-8')
        m, mark, waited = wait_for(pipe, r'\[reload\] published (\d+) variant\(s\) of (\d+) effect\(s\) in one step', mark, timeout=180)
        text = pipe.log_text()
        rebuilt_lines = len(re.findall(r'\[reload\] yBG_\w+: \d+ variant\(s\) rebuilt', text[mark - 4000 if mark > 4000 else 0:]))
        shot('E')
        report('viewer: a shared include edit is published in ONE step for all its effects', bool(m) and int(m.group(2)) > 1,
               f'{m.group(1)} variants of {m.group(2)} effects in one step after {waited:.1f} s' if m else f'no "published ... in one step" line within {waited:.0f} s')
    finally:
        pipe.stop()
        report('viewer: exits cleanly with the watcher running (Shutdown before teardown)', pipe.proc.returncode in (0, None) and pipe.proc.poll() is not None, f'exit code {pipe.proc.returncode}')


# ---- fview -----------------------------------------------------------------------------------------------------------------------

def run(cmd, timeout=900):
    # Other lanes' viewers / players share the host-visible device-local heap (the renderer's frame ring): when it is full, fview says so at open; wait and ask again.
    for attempt in range(24):
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        out = p.stdout + p.stderr
        if 'Not enough graphics memory' not in out:
            return p.returncode, out
        time.sleep(5)
    return p.returncode, out


def fview_part(root, main, work):
    sys.path.insert(0, str(root / 'tools' / 'fview'))   # lane 218: the shared locator (refuses an exe older than the renderer sources)
    import fview_exe as fe
    try:
        exe = Path(fe.find_fview(root=root, kind='dev'))
    except fe.FviewError as e:
        report('fview: exe', False, str(e))
        return
    cards = main / 'out' / 'captures' / 's100cards'
    
    for need in (exe, cards):
        if not need.exists():
            report('fview: inputs', False, f'cannot read {need}')
            return
    pristine = root / 'src' / 'renderer' / 'shaders' / 'fx'
    # The far-crowd frame of the capture (frame 1, the scene colour as of the op before the post passes): lane 2's own coordinates for it.
    cfg = json.loads((root / 'tools' / 'fview' / 'selftest_cards.json').read_text())
    surface, through_op = cfg['surface'], cfg['through_op']
    query = [str(exe), '--root', str(root), '--capture', str(cards), '--frame', str(cfg['frame']), '--surface', str(surface)]
    base = query + ['--op', str(through_op), '--validation', '1']

    def picture(extra, label, fx=pristine):
        code, out = run(base + ['--fx-dir', str(fx)] + extra + ['--save-case'])
        m = re.search(r'case saved: (.+)', out)
        if not m or not (Path(m.group(1).strip()) / 'picture.png').exists():
            return code, out, None
        return code, out, read_png(Path(m.group(1).strip()) / 'picture.png')

    fx_edit = work / 'fview_fx_edit'
    shutil.copytree(pristine, fx_edit)
    # Only the crowd's main variant (lm_pixel_color_alpha, a game shader of its own). The other variants of the file are not edited: one of them (color_alpha) is the
    # SAME game shader as yBG_UVScroll / yProp_UVScroll / ySprite, so an edit there legitimately moves those effects' pixels too (found by the first run of this test).
    multiply_edit(fx_edit / 'yAudience.frag', 'defined(FX_LM_PIXEL) && defined(FX_COLOR_ALPHA)')
    fx_broken = work / 'fview_fx_broken'
    shutil.copytree(pristine, fx_broken)
    break_edit(fx_broken / 'yAudience.frag')

    code, out, a = picture([], 'A')
    if a is None:
        report('fview: picture A', False, f'exit {code}: {out[-400:]}')
        return
    report('fview: picture A (validation 0)', code == 0 and 'validation errors 0' in out, f'{a[0]}x{a[1]}')

    t0 = time.time()
    code, out, b = picture(['--apply-reload', 'yAudience'], 'B', fx_edit)
    t_apply = time.time() - t0
    if b is None:
        report('fview: edit -> new picture', False, f'exit {code}: {out[-500:]}')
        return
    equal, count, channels_ok, changed, bright_same = compare(a, b)
    report('fview: edit -> new picture (red down, green / blue untouched)', code == 0 and (not equal) and channels_ok and count > 0,
           f'{count} pixels changed, green/blue untouched and red only down: {channels_ok}; one process start + reload + bind + picture took {t_apply:.1f} s')

    # fview's own per-pixel names: the top draw that wrote a pixel (as of the edited run)
    def top_effect(x, y):
        out_json = work / f'px_{x}_{y}.json'
        # (asked of the EDITED run: a pixel that moved was last written, in that run, by a yAudience draw; one that did not move was not)
        c, o = run(query + ['--fx-dir', str(fx_edit), '--apply-reload', 'yAudience', '--through-op', str(cfg['through_op']), '--pixel', f'{x},{y}', '--json', str(out_json)])
        h = json.loads(out_json.read_text())
        return h['entries'][h['top_wrote_index']]['effect']

    step_c = max(1, len(changed) // 4)
    sample_changed = changed[::step_c][:4]
    step_s = max(1, len(bright_same) // 4)
    sample_same = bright_same[::step_s][:4]
    names_changed = [top_effect(x, y) for x, y in sample_changed]
    names_same = [top_effect(x, y) for x, y, _ in sample_same]
    report('fview: the pixels that moved are exactly yAudience\'s draws (pixel history names)', names_changed and all(n == 'yAudience' for n in names_changed) and
           (not names_same or all(n != 'yAudience' for n in names_same)), f'changed {sample_changed} -> {names_changed}; unchanged bright {[(x, y) for x, y, _ in sample_same]} -> {names_same}')

    code, out, c = picture(['--apply-reload', 'yAudience'], 'C', pristine)
    report('fview: revert (reload from the pristine sources) == picture A', c is not None and compare(a, c)[0], f'exit {code}')

    code, out, d = picture(['--apply-reload', 'yAudience', '--reload-continue'], 'D', fx_broken)
    m = re.search(r'FAILED[^\n]*?(yAudience\.frag:\d+)', out)
    report('fview: a syntax error names file:line and the picture stays', d is not None and compare(a, d)[0] and bool(m), f'{m.group(1) if m else "no file:line in the output: " + out[-300:]}; picture == A: {d is not None and compare(a, d)[0]}')

    # the folder override: the compiler writes the edit into a folder, fview loads it
    folder = work / 'fview_folder'
    code, out = run([sys.executable, '-I', str(root / 'tools' / 'fview' / 'reload_effect.py'), '--effect', 'yAudience', '--fx-dir', str(fx_edit), '--out', str(folder), '--write-layouts'])
    n_spv = len(list(folder.glob('*.spv'))) if folder.exists() else 0
    code2, out2, e = picture(['--shader_dir', str(folder)], 'E')
    report('fview: --shader_dir <folder> == picture B', n_spv > 0 and e is not None and compare(b, e)[0] and 'variant(s) overridden' in out2, f'{n_spv} spv files, folder load line present: {"variant(s) overridden" in out2}')

    # planted fault: one layout json says a different block size -> refused -> the compiled program is used -> picture == A
    victim = folder / 'yAudience_lm_pixel_color_alpha_fragment.json'  # the edited variant
    if not victim.exists():
        report('fview: planted layout fault', False, 'no layout json in the folder')
    else:
        spec = json.loads(victim.read_text())
        spec['block_bytes'] += 16
        victim.write_text(json.dumps(spec))
        code3, out3, f = picture(['--shader_dir', str(folder)], 'F')
        ok = f is not None and 'REFUSED' in out3 and compare(a, f)[0]
        report('fview: planted fault (the edited variant has a changed layout) is REFUSED, the compiled variant draws: picture == A', ok, f'{victim.name}: "REFUSED" in the output: {"REFUSED" in out3}; picture == A: {f is not None and compare(a, f)[0]}')

    # a file that is not SPIR-V (right size, wrong magic) is refused at the folder load and the compiled variant draws
    junk = work / 'fview_junk'
    junk.mkdir()
    (junk / 'yAudience_lm_pixel_color_alpha_fragment.spv').write_bytes(b'\x00' * 64)
    code4, out4, g = picture(['--shader_dir', str(junk)], 'G')
    report('fview: a .spv without the SPIR-V magic is refused ("cannot read") and the compiled variant draws', g is not None and 'cannot read' in out4 and compare(a, g)[0],
           f'"cannot read" in the output: {"cannot read" in out4}; picture == A: {g is not None and compare(a, g)[0]}')

    # the game's alpha test must follow the description: "alpha_test": false in the rebuilt effect -> no twin (marker file) -> the plain variant draws ("program OFF");
    # the same folder built from the unchanged description keeps the twin ("program ON"). The pixel is lane 2's far-crowd card (yAudience, game alpha test ON).
    px = [str(cfg['x']) + ',' + str(cfg['y'])] if 'x' in cfg else []
    pixel_q = [str(exe), '--root', str(root), '--capture', str(cards), '--frame', str(cfg['frame']), '--surface', str(surface), '--through-op', str(through_op), '--pixel', px[0] if px else '0,0']
    fx_noalpha = work / 'fview_fx_noalpha'
    shutil.copytree(pristine, fx_noalpha)
    jp = fx_noalpha / 'yAudience.fx.json'
    spec = json.loads(jp.read_text(encoding='utf-8'))
    for v in spec['fragment']:
        v['alpha_test'] = False
    jp.write_text(json.dumps(spec, indent=1), encoding='utf-8')
    verdicts = {}
    for label, fxdir in (('twin', pristine), ('notwin', fx_noalpha)):
        folder2 = work / f'fview_folder_{label}'
        run([sys.executable, '-I', str(root / 'tools' / 'fview' / 'reload_effect.py'), '--effect', 'yAudience', '--fx-dir', str(fxdir), '--out', str(folder2), '--write-layouts'])
        code5, out5 = run(pixel_q + ['--fx-dir', str(pristine), '--shader_dir', str(folder2)])
        verdicts[label] = ('program OFF' if 'game ON, program OFF' in out5 else 'program ON' if 'game ON, program ON' in out5 else 'none: ' + out5[-200:], len(list(folder2.glob('*.notwin'))))
    report('fview: "alpha_test": false in the rebuilt effect drops the twin (folder marker): the pixel reads game ON, program OFF; unchanged description keeps program ON',
           verdicts['notwin'][0] == 'program OFF' and verdicts['twin'][0] == 'program ON' and verdicts['notwin'][1] > 0 and verdicts['twin'][1] == 0, f'{verdicts}')



def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=str(ROOT))
    ap.add_argument('--only', default='')
    ap.add_argument('--keep', action='store_true', help='keep the work folder (pictures, logs) even when everything passed')
    args = ap.parse_args()
    root = Path(args.root).resolve()
    main_checkout = root if (root / 'out' / 'captures').exists() else root.parents[2]
    (root / 'out' / 's101' / 'reload').mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='reload_selftest_', dir=str(root / 'out' / 's101' / 'reload')))
    print(f'work folder: {work}')
    try:
        if args.only in ('', 'viewer'):
            viewer_part(root, main_checkout, work)
        if args.only in ('', 'fview'):
            fview_part(root, main_checkout, work)
    except Exception as error:  # an input that cannot be read is a failure, never a pass
        report('selftest crashed', False, repr(error))
    bad = [n for n, ok in results if not ok]
    print(f'{len(results) - len(bad)} PASS, {len(bad)} FAIL')
    if not bad and results and not args.keep:
        shutil.rmtree(work, ignore_errors=True)
    return 1 if bad or not results else 0


if __name__ == '__main__':
    sys.exit(main())
