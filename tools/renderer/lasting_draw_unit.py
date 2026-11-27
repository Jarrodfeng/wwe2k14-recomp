#!/usr/bin/env python3
"""S102 T3: builds and runs src\\renderer_adapter\\lasting_draw_test.cpp (GPU free, 0 game runs), then its four planted twins, each of which must FAIL.
  py -I tools\\renderer\\lasting_draw_unit.py [--root DIR]
Then the wiring lints: the switches are in the table (rdr_switches.h) and the cvars are defined (reader.h), live.cpp reads them through BoolSeen / IntSeen.
Exit 0 = pass, 1 = fail."""
import argparse, os, subprocess, sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    ra = os.path.join(root, 'src', 'renderer_adapter')
    out = os.path.join(root, 'out', 't3')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'lasting_draw_test.exe')
    b = subprocess.run([CLANG, '-std=c++20', '-Wall', '-Wextra', '-I', os.path.join(root, 'src'), '-I', ra, os.path.join(ra, 'lasting_draw_test.cpp'), '-o', exe],
                       capture_output=True, text=True)
    if b.returncode:
        print('FAIL: did not compile\n' + b.stdout + b.stderr)
        return 1
    r = subprocess.run([exe], capture_output=True, text=True)
    print(r.stdout.strip())
    if r.returncode:
        print('FAIL: the unit test failed')
        return 1
    for plant in (1, 2, 3, 4, 5, 6):
        p = subprocess.run([exe, str(plant)], capture_output=True, text=True)
        if p.returncode == 0:
            print('FAIL: planted twin %d was not caught' % plant)
            return 1
        print('twin %d: caught' % plant)
    # wiring lints (read the sources; a missing piece fails loudly)
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'split'))
    from read_unit import read_path  # a split file is read with its parts expanded (tools/split/read_unit.py)

    def text(rel):
        return read_path(os.path.join(root, rel))
    sw, rd, lv = text('src/renderer_adapter/rdr_switches.h'), text('src/renderer_adapter/reader.h'), text('src/renderer_adapter/live.cpp')
    need = [
        (sw, 'X(LastingDraw, "wwe2k14_rdr_lasting_draw", PerFrame, 0)', 'switch table: lasting_draw per-frame default off'),
        (sw, 'X(LastingDrawPct, "wwe2k14_rdr_lasting_draw_pct", PerFrame, 50)', 'switch table: lasting_draw_pct per-frame default 50'),
        (rd, 'REXCVAR_DEFINE_BOOL(wwe2k14_rdr_lasting_draw, false', 'reader.h defines the switch (default false)'),
        (rd, 'REXCVAR_DEFINE_INT32(wwe2k14_rdr_lasting_draw_pct, 50', 'reader.h defines the threshold (default 50)'),
        (lv, 'rdr_sw::BoolSeen(rdr_sw::kLastingDraw', 'live.cpp reads the switch per frame'),
        (lv, 'rdr_sw::IntSeen(rdr_sw::kLastingDrawPct', 'live.cpp reads the threshold per frame'),
        (lv, 'lasting_draw::Resolve(', 'live.cpp uses the tested verdict'),
        (lv, 'if (!skip_wanted || lasting_drawn_now) {', 'live.cpp: the draw path runs when not skipped or drawn instead'),
        (lv, 'if (lasting_verdict.owe_missed) for (const rdr::CopyOp& c : left_out_copies) {', 'live.cpp: NoteMissed only for a frame that owes its copies'),
        (lv, 'in.waiting = uint32_t(waiting);', 'live.cpp: the queue-bound inputs reach the decision'),
    ]
    # F3: both switches are read before `if (skip_wanted)` (every frame), not only inside it
    i_read, i_skip = lv.find('rdr_sw::BoolSeen(rdr_sw::kLastingDraw'), lv.find('if (skip_wanted) {')
    ok = 0 <= i_read < i_skip and 0 <= lv.find('rdr_sw::IntSeen(rdr_sw::kLastingDrawPct') < i_skip
    print(('ok   ' if ok else 'FAIL ') + 'live.cpp reads both switches every frame, before the skip branch')
    if not ok:
        return 1
    bad = 0
    for src, needle, what in need:
        ok = needle in src
        print(('ok   ' if ok else 'FAIL ') + what)
        bad += not ok
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
