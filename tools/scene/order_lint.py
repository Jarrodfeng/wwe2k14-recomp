#!/usr/bin/env python3
"""S103 FOB lint (reviews 1 and 4): the stage families' row order and the miss rule, read from the source text (no build, no game).
  1. Classes() (record_frame_classes.cpp) lists the families in the game's order (docs\\research\\s103\\XFO.md G1-G7): ring_early, arena, screens, ring_late, props, crowd, wrestlers, models, flashes;
     every c::kCRows[N] index appears exactly once; kCRows (rf_arena_live.cpp) is ring_early, arena, screens, ring_late, props, flashes.
  2. c_driver's default row_order (the panels' order) is the order Classes() uses for the kCRows rows.
  3. A class applier that returns `!miss` (a not-resident arena) must reach EnsureArena first, and EnsureArena itself must note the miss (ModelCache::NoteMiss) on its not-resident path: the
     frame is then a LOADING miss whichever row runs first (the S103 L31A fix; FOB review 1: ring_early runs before the arena row).
  py -I tools\\scene\\order_lint.py [--root .] [--self-test]      exit 1 on any problem; --self-test plants each fault and requires the lint to catch it."""
import argparse
import os
import re
import sys

WANT_CLASSES = ['ring_early', 'arena', 'screens', 'ring_late', 'props', 'crowd', 'wrestlers', 'models', 'flashes']
WANT_ROWS = ['ring_early', 'arena', 'screens', 'ring_late', 'props', 'flashes']


def read(root, rel):
    return open(os.path.join(root, rel), encoding='utf-8', errors='replace').read()


def lint(src):
    """src: dict rel path -> text. -> list of problems."""
    bad = []
    rows = re.findall(r'\{"(\w+)",\s*Family::\w+,\s*&\w+\}', src['src/renderer_scene/rf_arena_live.cpp'].split('const ClassEntry kCRows')[1])
    if rows != WANT_ROWS:
        bad.append('kCRows is %s, want %s' % (rows, WANT_ROWS))
    cls = src['src/renderer_scene/record_frame_classes.cpp']
    body = cls.split('static const std::vector<ClassEntry> table = {')[1].split('};')[0]
    seq, idxs = [], []
    for line in body.splitlines():
        code = line.split('//')[0]
        m = re.search(r'c::kCRows\[(\d+)\]', code)
        if m:
            idxs.append(int(m.group(1)))
            seq.append(WANT_ROWS[int(m.group(1))] if int(m.group(1)) < len(WANT_ROWS) else '?')
            continue
        m = re.search(r'\{"(\w+)",\s*Family::', code)
        if m:
            seq.append(m.group(1))
    if sorted(idxs) != list(range(len(WANT_ROWS))):
        bad.append('Classes() uses kCRows indices %s, want each of 0..%d exactly once' % (idxs, len(WANT_ROWS) - 1))
    if seq != WANT_CLASSES:
        bad.append('Classes() order is %s, want the game\'s %s' % (seq, WANT_CLASSES))
    drv = src['tools/scene/c_driver.cpp']
    m = re.search(r'return \{(\d+(?:, \d+)*)\};\s*\n\s*\};', drv.split('row_order = ')[1]) if 'row_order = ' in drv else None
    default = [int(x) for x in m.group(1).split(', ')] if m else None
    if default != idxs:
        bad.append('c_driver default row_order %s != the kCRows order Classes() uses %s' % (default, idxs))
    core = src['src/renderer_scene/rf_c_core.cpp']
    mm = re.search(r'miss = true;([^\n]*\n){1,3}', core)
    if not mm or 'NoteMiss' not in mm.group(0):
        bad.append('rf_c_core.cpp: EnsureArena does not NoteMiss right after `miss = true;` (a loading arena would be a strike for every row that runs before the arena row)')
    for rel, text in src.items():
        if not re.search(r'src/renderer_scene/(rf_\w+|live_flashes)\.cpp$', rel):
            continue
        for fn in re.finditer(r'^(?:bool|static bool)\s+(Apply\w+|RingRun)\(BuildCtx& ctx[^)]*\)\s*\{(.*?)^\}', text, re.S | re.M):
            b = fn.group(2)
            for r in re.finditer(r'return !miss;', b):
                if 'EnsureArena(' not in b[:r.start()] and 'NoteMiss' not in b[:r.start()]:
                    bad.append('%s %s: `return !miss` without EnsureArena / NoteMiss before it' % (rel, fn.group(1)))
    return bad


FILES = ['src/renderer_scene/rf_arena_live.cpp', 'src/renderer_scene/record_frame_classes.cpp', 'tools/scene/c_driver.cpp', 'src/renderer_scene/rf_c_core.cpp',
         'src/renderer_scene/rf_ring.cpp', 'src/renderer_scene/rf_screens.cpp', 'src/renderer_scene/rf_props.cpp', 'src/renderer_scene/live_flashes.cpp']


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--root', default='.')
    ap.add_argument('--self-test', action='store_true')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    src = {f: read(root, f) for f in FILES}
    bad = lint(src)
    for b in bad:
        print('FAIL  ' + b)
    if not a.self_test:
        print('order_lint:', 'FAIL (%d)' % len(bad) if bad else 'PASS')
        return 1 if bad else 0
    if bad:
        print('self-test needs the clean tree to pass first')
        return 1
    plants = [
        ('no NoteMiss in EnsureArena (the old RingRun shape)', 'src/renderer_scene/rf_c_core.cpp', lambda t: re.sub(r'[^\n]*NoteMiss\(\);[^\n]*\n', '', t, 1)),
        ('Classes(): ring before the arena swapped back (old order)', 'src/renderer_scene/record_frame_classes.cpp', lambda t: t.replace('c::kCRows[0],', 'c::kCRowsTMP,').replace('c::kCRows[1],', 'c::kCRows[0],').replace('c::kCRowsTMP,', 'c::kCRows[1],')),
        ('c_driver default row_order changed', 'tools/scene/c_driver.cpp', lambda t: t.replace('return {0, 1, 2, 3, 4, 5};', 'return {1, 0, 2, 3, 4, 5};')),
        ('a kCRows index used twice', 'src/renderer_scene/record_frame_classes.cpp', lambda t: t.replace('c::kCRows[5],', 'c::kCRows[4],')),
        ('an applier returning !miss without EnsureArena', 'src/renderer_scene/rf_screens.cpp', lambda t: t.replace('ArenaSet* as = EnsureArena(ctx, why, miss);', 'ArenaSet* as = nullptr; (void)why;', 1)),
    ]
    rc = 0
    for name, rel, fn in plants:
        mut = dict(src)
        mut[rel] = fn(src[rel])
        got = lint(mut) if mut[rel] != src[rel] else []
        ok = bool(got)
        rc |= 0 if ok else 1
        print('%s  plant "%s": %s' % ('PASS' if ok else 'FAIL', name, 'caught' if ok else 'NOT CAUGHT'))
    print('order_lint self-test:', 'FAIL' if rc else 'PASS')
    return rc


if __name__ == '__main__':
    sys.exit(main())
