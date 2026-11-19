#!/usr/bin/env python3
"""S103 L1C lint (Opus review B1): no scene_inputs::Capture inside any contributor's Emit.

The record's input_stamps snapshot is taken at the END OF THE LATCH, before Emit runs (live_scene.cpp LatchImpl stamp_fill; BuildFrame copies it). A stamp made inside an Emit is not in the record's own
snapshot, so the render side's freshness check ("record inputs not fresh") refuses the OURS record: misses, strikes, then latch-off. Run 32 would have drawn no OURS frame (bits 7 and 17 were stamped at Emit).
Stamp at the latch (Latch() / the Take it calls) instead. The ONE allowed exception is a bit named in scene_inputs::kEmitStampedBits (inputs_core.h: bit 14, screens, which needs the record's own movie
rows); live_scene.cpp's SetEmitStampFill overlays exactly those rows after the emit. This lint allows `Capture(...kInStageScreens...)` inside an Emit and nothing else.

  py -I tools\\scene\\emit_stamp_lint.py [--root DIR] [--selftest]
Exit 0 = pass. --selftest plants a Capture(kInStageClock) inside an Emit and must FAIL it."""
import argparse
import glob
import os
import re
import sys

ALLOWED = ('kInStageScreens',)
EMIT_RE = re.compile(r'\b(?:\w+::)?Emit\s*\([^;{)]*\)[^;{]*\{')
CAPTURE_RE = re.compile(r'\bCapture\s*\(([^;]*)\)\s*;')


def strip_comments(s):
    s = re.sub(r'/\*.*?\*/', lambda m: re.sub(r'[^\n]', ' ', m.group(0)), s, flags=re.S)
    return re.sub(r'//[^\n]*', lambda m: ' ' * len(m.group(0)), s)


def emit_bodies(text):
    """Yield (start_line, body) for every Emit definition (a function with a body)."""
    t = strip_comments(text)
    for m in EMIT_RE.finditer(t):
        i = m.end()
        depth = 1
        while i < len(t) and depth:
            depth += (t[i] == '{') - (t[i] == '}')
            i += 1
        yield t.count('\n', 0, m.start()) + 1, t[m.end():i]


def scan_text(name, text):
    bad = []
    for line, body in emit_bodies(text):
        for c in CAPTURE_RE.finditer(body):
            if not any(a in c.group(1) for a in ALLOWED):
                bad.append('%s:%d: Capture(%s) inside an Emit' % (name, line + body.count('\n', 0, c.start()), c.group(1).strip()))
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        planted = 'void Emit(const FrameInfo& fi, SceneTables& t, Counters& c) override {\n  if (x) scene_inputs::Capture(scene_inputs::kInStageClock, 1);\n}\n'
        clean = 'void Emit(const FrameInfo& fi, SceneTables& t, Counters& c) override {\n  scene_inputs::Capture(scene_inputs::kInStageScreens, 1);\n}\nbool Latch(int) { scene_inputs::Capture(scene_inputs::kInStageClock, 1); }\n'
        ok = len(scan_text('planted', planted)) == 1 and not scan_text('clean', clean)
        print('selftest: planted twin %s, clean sample %s' % ('caught' if len(scan_text('planted', planted)) == 1 else 'NOT CAUGHT', 'passes' if not scan_text('clean', clean) else 'FAILS'))
        return 0 if ok else 1
    bad = []
    n = 0
    for p in sorted(glob.glob(os.path.join(a.root, 'src', 'scene_live', '*.h')) + glob.glob(os.path.join(a.root, 'src', 'scene_live', '*.cpp'))):
        with open(p, encoding='utf-8', errors='replace') as f:
            text = f.read()
        n += 1
        bad += scan_text(os.path.relpath(p, a.root), text)
    for b in bad:
        print('FAIL: ' + b)
    print(('FAIL' if bad else 'PASS') + ': emit_stamp_lint: %d scene_live files, %d Capture(s) inside an Emit' % (n, len(bad)))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
