#!/usr/bin/env python3
"""cvar_link_lint (lane LNK, 2026-11-20; docs\\research\\harness\\LNK_contract.md): every cvar a game build READS is DEFINED by exactly one unit of that build, over the whole tree.

The class: a switch declared (REXCVAR_DECLARE, only a function declaration) and read (REXCVAR_GET / SET) in one game build that never compiles its REXCVAR_DEFINE_*: an unresolved
symbol at link time, which the syntax check (tools\\compile\\game_syntax.py) cannot see. Found 2026-11-20: wwe2k14_rdr_hide_stage_slot / _guest_flat / _detached_pieces / _spring_hair_rows
were defined only in start_settings.cpp's `#if !WWE2K14_STREAM()` block, so the stream (ON) build did not link.

The judge is the hand-back check's own (tools\\handback\\check.py cvar_findings), and since LNK round 3 it asks the REAL preprocessor: every game unit of each build (the tree's
CMakeLists.txt `set(WWE2K14_SOURCES ...)` + `target_sources(wwe2k14 ...)` and the keep_stream lists) is run through clang -E -fdirectives-only with its own build flags
(tools\\handback\\cvar_pp.py; the command lines are game_syntax.py's), and the cvar uses are read from that text. Rules: `undeclared` (a read the unit never declares), `link` (a
read no unit of the build defines), `duplicate` (two units of one build define it). A unit that cannot be preprocessed is BLOCKED (exit 2), never clean. Cold about 20 s
(186 unit-builds), warm about 1-2 s (a cache in the main checkout's out\\cvar_pp).

  py -I tools\\scene\\cvar_link_lint.py [--root .]     judge the tree (exit 1 on a finding, 2 when a unit or the whole judge is BLOCKED)
  py -I tools\\scene\\cvar_link_lint.py --selftest      the planted twins (clang VFS overlays on the real tree: nothing is written to src\\; each MUST be caught) and the real tree
                                                      (MUST be clean):
      1 the four LNK switches' stream definitions dropped (S110: from their src\\cvars shards; before, from reader.h; their pre-fix state): exactly their four ON link findings
      2 ... moved into a header that reader.h includes only under `#ifdef LNK_NEVER_DEFINED`: the same four ON link findings
      3 ... moved into a header only host_start.cpp (an OFF unit) includes: the four ON link findings and OFF duplicates of the same four
      4 live.cpp (a second ON unit) includes a header that defines the four again: their four ON duplicates
"""
# entry: cvar_findings
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'handback'))
import cvar_pp as hb  # noqa: E402  (tools\handback\cvar_pp.py: cvar_findings)

LNK_FOUR = ('wwe2k14_rdr_hide_stage_slot', 'wwe2k14_rdr_guest_flat', 'wwe2k14_rdr_detached_pieces', 'wwe2k14_rdr_spring_hair_rows')
LNK_HOME = 'src/renderer_adapter/reader.h'
PLANTED = 'src/renderer_adapter/lnk_planted.h'


def link_findings(root, overlay=None, notes=None):
    """([(key, message)] of the undeclared / link / duplicate rules for every cvar, rows, blocked). `overlay` {path relative to root: planted text}."""
    ov = {os.path.join(root, k): v for k, v in (overlay or {}).items()}
    return hb.cvar_findings(root, None, notes, overlay=ov or None)


def _read(root, rel):
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'split'))
    from read_unit import read_path  # a split file is read with its parts expanded (tools/split/read_unit.py)
    return read_path(os.path.join(root, rel), crlf=True)


def _split_four(root):
    """({generated shard path: its text without the four LNK definitions}, the four definitions' text). S110 CVARSHARD: reader.h only declares them; they sit in src\\cvars\\cvars_<n>.cpp."""
    sys.path.insert(0, os.path.join(HERE, '..', 'compile'))
    import cvar_shard
    shards, four = {}, []
    for i in range(cvar_shard.SHARDS):
        rel = cvar_shard.shard_path(i)
        text = _read(root, rel)
        spans = [(s.start, s.end, s.text) for s in cvar_shard.scan_text(text)[1] if s.name in LNK_FOUR and not s.skip]
        for a, b, t in reversed(spans):
            text = text[:a] + text[b:]
        four += [t + '\n' for _a, _b, t in spans]
        shards[rel] = text
    if len(four) != 4:
        raise SystemExit('selftest: the shards do not hold the four LNK definitions once each (%d found): the twins need updating' % len(four))
    return shards, ''.join(four)


def twins(root):
    shards, four = _split_four(root)
    reader = _read(root, LNK_HOME)
    host = _read(root, 'src/renderer_host/host_start.cpp')
    live = _read(root, 'src/renderer_adapter/live.cpp')
    planted = '#pragma once\n#include <rex/cvar.h>\n' + four
    return [
        ('the four LNK switches without their stream definitions', dict(shards), 'link', LNK_FOUR),
        ('their definitions in a header reader.h includes only under #ifdef LNK_NEVER_DEFINED',
         dict(shards, **{LNK_HOME: reader + '\n#ifdef LNK_NEVER_DEFINED\n#include "lnk_planted.h"\n#endif\n', PLANTED: planted}), 'link', LNK_FOUR),
        ('their definitions in a header only host_start.cpp (an OFF unit) includes',
         dict(shards, **{PLANTED: planted, 'src/renderer_host/host_start.cpp': host + '\n#include "renderer_adapter/lnk_planted.h"\n'}),   # (a `..` path does not cross the VFS overlay)
         'link', LNK_FOUR, ('OFF', 'duplicate')),
        ('live.cpp (a second ON unit) defines the four again (a header it includes)', {PLANTED: planted, 'src/renderer_adapter/live.cpp': live + '\n#include "lnk_planted.h"\n'}, 'duplicate', LNK_FOUR),
    ]


def _case(name, res, want_kind, want_names, also=None):
    got, _rows, blocked = res
    hit = sorted({k[2] for k, _m in got if k[1] == 'ON' and k[3] == want_kind})
    if want_names is not None:   # other findings may come with it, but only about these names
        ok = hit == sorted(want_names) and all(k[2] in want_names for k, _m in got)
        if also:   # twin 3: start_settings.cpp's stream-free copies and host_start.cpp's planted header are OFF duplicates of exactly the four
            ok = ok and sorted({k[2] for k, _m in got if (k[1], k[3]) == also}) == sorted(want_names)
    else:
        ok = len(hit) >= 50 and set(LNK_FOUR) <= set(hit) and all(k[3] == want_kind for k, _m in got)
    ok = ok and not blocked
    shown = ', '.join(hit) if len(hit) <= 6 else '%s ... (%d names)' % (', '.join(hit[:4]), len(hit))
    other = sorted({'%s %s' % (k[1], k[3]) for k, _m in got if not (k[1] == 'ON' and k[3] == want_kind)})
    print('%s  planted: %s -> %d finding(s), ON %s names %s%s%s' % ('ok  ' if ok else 'FAIL', name, len(got), want_kind, shown or 'none',
                                                                ('; also ' + ', '.join(other)) if other else '', ('; BLOCKED %d' % len(blocked)) if blocked else ''))
    if got:
        print('        e.g. ' + got[0][1][:240])
    return not ok


def selftest(root):
    bad = 0
    for twin in twins(root):
        name, overlay, kind, names = twin[:4]
        bad += _case(name, link_findings(root, overlay), kind, names, twin[4] if len(twin) > 4 else None)
    notes = []
    got, rows, blocked = link_findings(root, notes=notes)
    ok = not got and not blocked and rows > 0
    print('%s  real tree: %d (unit, mode) reads, %d finding(s), %d unit-build(s) blocked, %d not judged' % ('ok  ' if ok else 'FAIL', rows, len(got), len(blocked), len(notes)))
    bad += not ok
    print('PASS: every planted twin is caught, the tree is clean' if not bad else 'FAIL: %d selftest case(s)' % bad)
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--root', default=os.path.normpath(os.path.join(HERE, '..', '..')))
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    try:
        if a.selftest:
            return selftest(root)
        notes = []
        got, rows, blocked = link_findings(root, notes=notes)
    except RuntimeError as e:
        print('BLOCKED: the cvar judge cannot preprocess the game units: %s' % e)
        return 2
    for n in notes:
        print('note: ' + n)
    for u, m, e in blocked:
        print('BLOCKED: %s [%s]: %s' % (u, 'ON' if m else 'OFF', e))
    for _k, m in got:
        print('error: ' + m)
    print('cvar_link_lint: %d (unit, mode) reads, %d finding(s), %d unit-build(s) blocked, %d not judged' % (rows, len(got), len(blocked), len(notes)))
    return 1 if got else 2 if blocked else 0


if __name__ == '__main__':
    sys.exit(main())
