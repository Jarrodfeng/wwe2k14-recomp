r"""S102 lane KO: the offline proof of the three knock-out switches (src\renderer_adapter\knockouts.h). 0 game runs, 0 Surface runs: rplay on the Bryan capture.

  py -I tools\ko\ko_check.py [--capture DIR] [--frames N] [--rplay EXE]                 draw_none + render_drop on rplay, and the source lint
  py -I tools\ko\ko_check.py ... --plant                                                 the planted twin: --ko-draw-none --ko-plant (every second frame still
                                                                                         records its passes). Exit 0 only when the check FAILED naming the frames.
  py -I tools\ko\ko_check.py --lint-only                                                 only the source lint (and its planted twin)

What is proven:
  control      rplay --counts without a switch: every frame has passes > 0 and draws > 0 (the counts file can see a draw).
  draw_none    rplay --ko-draw-none --counts: every frame has passes == 0 and draws + indexed_draws == 0 (the M1 counts), steps > 0 (the present step is
               still planned), dropped == 0, and the frame count is N. GPU work per frame = the present and the uploads only.
  render_drop  rplay --ko-render-drop --timing: every row has draws, bind_us, render_us, gpu_us == 0, N rows, and the log says N of N frames dropped.
  lint         every reader hook outside reader.h goes through rdr_reader::ReaderOn() (no raw REXCVAR_GET(wwe2k14_rdr_capture) in half2_hooks.h /
               d3d_textures.h / d3d_layer_copy.h / thumb_defer.h), the present hook latches AFTER the reader's own OnPresent, and live.cpp reads both
               render-side switches through the S0-4 table (BoolSeen). Planted twin: a hook text with a raw read must be caught.
Exit 0 pass, 1 fail, 2 cannot run (missing capture / exe / unreadable counts: never a pass).
"""
import argparse, json, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parents[2]
CAPTURE = 'out/s101/lane92/case_20261112_122719_969_0_pc_run_exit/capture'
MAIN = Path(r'C:\Users\Jarrod\wwe2k14-recomp')
HOOK_FILES = ['src/half2_hooks.h', 'src/d3d_textures.h', 'src/d3d_layer_copy.h', 'src/thumb_defer.h']
RAW = 'REXCVAR_GET(wwe2k14_rdr_capture)'


class Bad(Exception):
    pass


def run_rplay(exe, capture, frames, extra, out_dir):
    cmd = [str(exe), '--capture', str(capture), '--frames', str(frames)] + extra
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=900)
    text = p.stdout + p.stderr
    if p.returncode != 0:
        raise Bad('rplay exit %d: %s' % (p.returncode, ' | '.join(text.strip().splitlines()[-4:])))
    return text


def load_counts(path, frames):
    try:
        d = json.loads(Path(path).read_text(encoding='utf-8'))
    except Exception as e:
        raise Bad('cannot read counts %s: %s' % (path, e))
    fr = d.get('frames')
    if d.get('format') != 'rdr-counts' or not isinstance(fr, list):
        raise Bad('%s is not a counts file' % path)
    if len(fr) != frames:
        raise Bad('counts file has %d frames, expected %d' % (len(fr), frames))
    return fr


def draws_of(f):
    t = f.get('total', {})
    return t.get('draws', 0) + t.get('indexed_draws', 0)


def check_control(fr):
    bad = [i for i, f in enumerate(fr) if not (f.get('passes', 0) > 0 and draws_of(f) > 0)]
    if bad:
        raise Bad('control: frames without passes / draws: %s (the counts cannot see a draw)' % bad[:8])


def check_draw_none(fr):
    problems = []
    for i, f in enumerate(fr):
        if f.get('passes', 0) != 0 or draws_of(f) != 0:
            problems.append('frame %d records passes=%d draws=%d' % (i, f.get('passes', 0), draws_of(f)))
        elif f.get('steps', 0) <= 0:
            problems.append('frame %d has no planned steps (no present)' % i)
        elif f.get('dropped', 0):
            problems.append('frame %d was dropped by the renderer' % i)
    if problems:
        raise Bad('draw_none: %d frames still draw: %s' % (len(problems), '; '.join(problems[:6])))


def check_render_drop(timing_path, frames, log):
    rows = Path(timing_path).read_text(encoding='utf-8').strip().splitlines()[1:]
    if len(rows) != frames:
        raise Bad('render_drop: timing has %d rows, expected %d' % (len(rows), frames))
    for r in rows:
        c = r.split(',')
        if int(c[1]) != 0 or float(c[3]) != 0 or float(c[4]) != 0 or float(c[5]) != 0:
            raise Bad('render_drop: a frame did bind / render work: %s' % r)
    if '%d of %d frames dropped' % (frames, frames) not in log:
        raise Bad('render_drop: rplay did not report %d of %d frames dropped' % (frames, frames))


def lint(texts):
    """texts: {name: text}. Returns a list of problems."""
    out = []
    for name in HOOK_FILES:
        t = texts.get(name)
        if t is None:
            out.append('%s unreadable' % name)
            continue
        if RAW in t:
            out.append('%s still reads %s directly (a reader hook outside ReaderOn)' % (name, RAW))
        if 'ReaderOn()' not in t:
            out.append('%s has no ReaderOn() gate' % name)
    h = texts.get('src/half2_hooks.h', '')
    a, b = h.find('rdr_reader::OnPresent(ctx, base)'), h.find('KoLatchAtPresent()')
    if a < 0 or b < 0 or b < a:
        out.append('half2_hooks.h: KoLatchAtPresent must come after the reader OnPresent')
    live = texts.get('src/renderer_adapter/live.cpp', '')
    for k in ('kKoRenderDrop', 'kKoDrawNone'):
        if 'rdr_sw::BoolSeen(rdr_sw::%s' % k not in live:
            out.append('live.cpp does not read %s through BoolSeen' % k)
    sw = texts.get('src/renderer_adapter/rdr_switches.h', '')
    for n, kind in (('wwe2k14_rdr_ko_draw_none', 'PerFrame'), ('wwe2k14_rdr_ko_render_drop', 'StartOnly'), ('wwe2k14_rdr_ko_reader_none', 'StartOnly')):
        if '"%s", %s, 0' % (n, kind) not in sw:
            out.append('rdr_switches.h has no %s default-off entry for %s' % (kind, n))
    return out


def read_texts():
    t = {}
    for n in HOOK_FILES + ['src/renderer_adapter/live.cpp', 'src/renderer_adapter/rdr_switches.h']:
        try:
            t[n] = (HERE / n).read_text(encoding='utf-8')
        except Exception:
            pass
    return t


def lint_main():
    texts = read_texts()
    probs = lint(texts)
    if probs:
        raise Bad('lint: ' + '; '.join(probs))
    twin = dict(texts)
    twin['src/d3d_textures.h'] = twin['src/d3d_textures.h'] + '\n  if (' + RAW + ') wwe2k14::rdr_reader::OnClear(ctx, base);\n'
    if not lint(twin):
        raise Bad('lint planted twin (a raw capture read in a hook file) was NOT caught')
    print('ok lint: 4 hook files gated by ReaderOn(), latch after OnPresent, 2 render-side reads through the table; planted raw read caught')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--capture', default=str(MAIN / CAPTURE))
    ap.add_argument('--frames', type=int, default=100)
    ap.add_argument('--rplay', default=str(HERE / 'out' / 'renderer' / 'dev' / 'rplay.exe'))
    ap.add_argument('--plant', action='store_true', help='planted twin: exit 0 only when the draw_none check FAILED')
    ap.add_argument('--lint-only', action='store_true')
    a = ap.parse_args()
    try:
        if a.lint_only:
            lint_main()
            return 0
        if not (Path(a.capture) / 'images.bin').is_file():
            raise Bad('capture not found: %s' % a.capture)
        if not Path(a.rplay).is_file():
            raise Bad('rplay not found: %s' % a.rplay)
        with tempfile.TemporaryDirectory() as td:
            td = Path(td)
            if a.plant:
                run_rplay(a.rplay, a.capture, a.frames, ['--ko-draw-none', '--ko-plant', '--counts', str(td / 'p.json')], td)
                try:
                    check_draw_none(load_counts(td / 'p.json', a.frames))
                except Bad as e:
                    print('ok planted twin caught: %s' % e)
                    return 0
                print('FAIL planted twin: draw_none passed although every second frame was recorded in full')
                return 1
            run_rplay(a.rplay, a.capture, a.frames, ['--counts', str(td / 'c.json')], td)
            check_control(load_counts(td / 'c.json', a.frames))
            run_rplay(a.rplay, a.capture, a.frames, ['--ko-draw-none', '--counts', str(td / 'd.json')], td)
            check_draw_none(load_counts(td / 'd.json', a.frames))
            log = run_rplay(a.rplay, a.capture, a.frames, ['--ko-render-drop', '--timing', str(td / 't.csv')], td)
            check_render_drop(td / 't.csv', a.frames, log)
            print('ok control: %d frames with passes and draws; draw_none: 0 passes, 0 draws, present planned in all %d; render_drop: %d of %d dropped, bind / render / gpu 0' % (a.frames, a.frames, a.frames, a.frames))
        lint_main()
        return 0
    except Bad as e:
        msg = str(e)
        print(('CANNOT RUN ' if msg.startswith(('rplay', 'capture', 'cannot')) else 'FAIL ') + msg)
        return 2 if msg.startswith(('rplay', 'capture', 'cannot')) else 1
    except subprocess.TimeoutExpired:
        print('CANNOT RUN rplay timed out')
        return 2


if __name__ == '__main__':
    sys.exit(main())
