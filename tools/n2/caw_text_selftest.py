#!/usr/bin/env python3
"""S101 lane 118 (N2 / L-6): the offline acceptance test of the editor's drawn-state reader, gate lane n2_caw_text. 0 game runs.

  py -I tools\\n2\\caw_text_selftest.py [--root DIR]

The inputs are the two kept CAW walks (Check B, names.h section 69): out\\s101\\runs\\20261112_122353_s101_checkB_caw_walk and ..._121944 (game data, kept
local, extracted once under out\\s101\\n2_editor\\). Needs out\\tools\\caw_text_test.exe (tools\\n2\\build_caw_text_test.ps1, the gate's n2_caw_text.build).

 1 pinned sequence    per walk: seq 76 on every frame; (title, rows, tab, hot) change at capture frames 0 / 37 / 104 / 107 / 117 / 233; 9 rows then 4 rows;
                      293 frames with list rows (the lane-117 loose rule), 292 of them with exactly one hot row; 287 settled (rows on the grid, one hot row)
 2 zips agree         the two walks give the same states frame for frame (guest frames differ by the offset only)
 3 C++ == Python      the shipped C++ reader (src/caw_text.h) and tools\\n2\\caw_read.py print the same line on every frame of both walks (flash included)
 4 past-bug fixture   NOT a re-find: tools\\n2\\fixtures\\checkb_peek_7078.json holds counts taken from SESSION101_SUMMARY.md (the peeked words seq+64, seq+96, the page
                      pointer had 1 value each over Check B's walk); the check shows the drawn page, tab dot and hot row DO change over the same walk
 5 planted twins      each must FAIL the acceptance of 1: all colours white; the PS bank read instead of the VS bank; the first threshold r >= 0.3;
                      section 69 removed (a real frame with the section emptied); (the two-hot-row state is the driver selftest's)  -- in BOTH readers
"""
import argparse
import glob
import json
import os
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import caw_common as C
import caw_read
from caw_state import read_state, is_hot as loose_is_hot

EXPECT_EVENTS = [0, 37, 104, 107, 117, 233]
LOOSE_FRAMES, LOOSE_ONE_HOT, SETTLED = 293, 292, 287
FIXTURE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'fixtures', 'checkb_peek_7078.json')


def events(states):
    out, prev = [], None
    for i, s in enumerate(states):
        key = (s['has_text'], s['seq76'], s['title'], s['complete'], s['hot'], s['tab'], s['dots'])
        if key != prev:
            out.append(i)
            prev = key
    return out


def acceptance(states):
    """The pinned expectations of section 1 as a problem list (empty = the reader passes). Used for the walks AND for every planted twin."""
    probs = []
    n_text = sum(1 for s in states if s['has_text'])
    if n_text != len(states):
        probs.append('no text on %d of %d frames' % (len(states) - n_text, len(states)))
        return probs
    bad76 = sum(1 for s in states if not s['seq76'])
    if bad76:
        probs.append('seq 76 not read on %d frames' % bad76)
    complete = [s for s in states if s['complete']]
    nohot = sum(1 for s in complete if s['hot_n'] != 1)
    if nohot:
        probs.append('%d settled-looking frames have %s hot rows' % (nohot, 'no / several'))
    one_hot = sum(1 for s in states if s['complete'] and s['hot_n'] == 1)
    if one_hot != SETTLED:
        probs.append('settled frames with exactly one hot row: %d, pinned %d' % (one_hot, SETTLED))
    ev = events(states)
    if ev != EXPECT_EVENTS:
        probs.append('state changes at %s, pinned %s' % (ev, EXPECT_EVENTS))
    return probs


def loose_counts(cap):
    geom = open(os.path.join(cap, 'geometry.bin'), 'rb').read()
    files = sorted(glob.glob(os.path.join(cap, 'frames', 'frame_*.bin')))
    rows_frames = one_hot = 0
    for p in files:
        gf, seen = read_state(cap, p, geom)
        rows = [c for d, t, x, y, c in seen if x == 144.0 and 100 <= y < 580]
        if rows:
            rows_frames += 1
            one_hot += 1 if sum(1 for c in rows if loose_is_hot(c)) == 1 else 0
    return rows_frames, one_hot


def cpp_states_from_lines(lines):
    """Parse the C++ line back into the comparison tuple fields (has_text, seq76, complete, hot, hot_n, tab, dots, n_rows, title)."""
    out = []
    for ln in lines:
        f = ln.split('\t')
        out.append(dict(has_text=f[2] == '1', seq76=f[4] == '1', complete=f[5] == '1', hot=int(f[6]), hot_n=int(f[7]), tab=int(f[8]), dots=int(f[10]), title=f[12]))
    return out


def stripped_copy(src_cap, dest):
    """A real capture folder of 5 frames whose section 69 entry is emptied (count 0): the frame as an older build (no run strings) would write it."""
    if os.path.isdir(dest):
        shutil.rmtree(dest)
    os.makedirs(os.path.join(dest, 'frames'))
    for name in ('geometry.bin', 'images.bin', 'textures.bin'):
        shutil.copyfile(os.path.join(src_cap, name), os.path.join(dest, name))
    for n in range(5):
        data = bytearray(open(os.path.join(src_cap, 'frames', 'frame_%06d.bin' % n), 'rb').read())
        sc = struct.unpack_from('<I', data, 12)[0]
        hit = 0
        for i in range(sc):
            o = 32 + i * 24
            sid = struct.unpack_from('<I', data, o)[0]
            if sid == 69:
                struct.pack_into('<Q', data, o + 8, 0)   # count := 0
                hit += 1
        if hit != 1:
            raise SystemExit('FAIL  frame %d has %d section-69 entries' % (n, hit))
        open(os.path.join(dest, 'frames', 'frame_%06d.bin' % n), 'wb').write(bytes(data))
    return dest


def shifted_copy(src_cap, dest, frame=117, row_y=166.0, dy=3.0):
    """A one-frame capture folder (the frame becomes frame 0) in which every glyph quad of the row at y = row_y is moved dy px down: a row off its slot."""
    if os.path.isdir(dest):
        shutil.rmtree(dest)
    os.makedirs(os.path.join(dest, 'frames'))
    for name in ('images.bin', 'textures.bin'):
        shutil.copyfile(os.path.join(src_cap, name), os.path.join(dest, name))
    geom = bytearray(open(os.path.join(src_cap, 'geometry.bin'), 'rb').read())
    fpath = os.path.join(src_cap, 'frames', 'frame_%06d.bin' % frame)
    b, gf, secs = caw_read.load(fpath)
    des, dc, do = secs[4]
    es, cnt, o = secs[69]
    lay = [struct.unpack_from('<IIII', b, secs[11][2] + i * secs[11][0]) for i in range(secs[11][1])]
    hit = 0
    for i in range(cnt):
        d = struct.unpack_from('<IIII', b, o + i * es)[0]
        t = struct.unpack_from(caw_read.DRAW_FMT, b, do + d * des)
        vo, lo, vc = t[11], t[10], t[15]
        x, y = struct.unpack_from('<2f', geom, vo)
        if abs(x - 144.0) < 0.6 and abs(y - row_y) < 0.6:
            stride = lay[lo][2]
            for v in range(vc):
                yy = struct.unpack_from('<f', geom, vo + v * stride + 4)[0]
                struct.pack_into('<f', geom, vo + v * stride + 4, yy + dy)
            hit += 1
    if not hit:
        raise SystemExit('FAIL  shifted_copy: no glyph draw at y %s in frame %d' % (row_y, frame))
    open(os.path.join(dest, 'geometry.bin'), 'wb').write(bytes(geom))
    shutil.copyfile(fpath, os.path.join(dest, 'frames', 'frame_000000.bin'))
    return dest, hit


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=C.REPO)
    args = ap.parse_args()
    main_dir = C.main_checkout()
    caps = {w: C.capture_dir(main_dir, w) for w in C.WALKS}
    states = {w: caw_read.read_all(caps[w]) for w in C.WALKS}

    # 1 the pinned sequence, per walk
    for w in C.WALKS:
        st = states[w]
        probs = acceptance(st)
        rows_frames, one_hot = loose_counts(caps[w])
        if (rows_frames, one_hot) != (LOOSE_FRAMES, LOOSE_ONE_HOT):
            probs.append('loose rule: %d frames with rows, %d with one hot row; pinned %d / %d' % (rows_frames, one_hot, LOOSE_FRAMES, LOOSE_ONE_HOT))
        shape = [(st[0]['title'], len(st[0]['rows']), st[0]['tab']), (st[117]['title'], len(st[117]['rows']), st[117]['tab'])]
        if [len(shape[0][0]) > 0, shape[0][1], shape[0][2], shape[1][1], shape[1][2]] != [True, 9, 3, 4, 2]:
            probs.append('pages: %s' % shape)
        hot_seq = [(i, st[i]['hot']) for i in EXPECT_EVENTS if st[i]['hot'] >= 0]
        if [h for _, h in hot_seq] != [1, 2, 0, 1]:
            probs.append('hot rows at the events: %s, pinned [1, 2, 0, 1]' % hot_seq)
        C.report('1 pinned sequence %s' % w[:15], not probs, '; '.join(probs) or '%d frames, seq 76 on all, changes at %s, %d with rows / %d one hot (loose), %d settled' % (len(st), EXPECT_EVENTS, rows_frames, one_hot, SETTLED))

    # 2 the two zips agree frame for frame
    a, b = states[C.WALKS[0]], states[C.WALKS[1]]
    strip = lambda s: {k: v for k, v in s.items() if k != 'guest'}
    diff = [i for i in range(min(len(a), len(b))) if strip(a[i]) != strip(b[i])]
    off = {a[i]['guest'] - b[i]['guest'] for i in range(min(len(a), len(b)))}
    C.report('2 zips agree', len(a) == len(b) and not diff and len(off) == 1, '%d frames, %d differ, guest offset %s' % (len(a), len(diff), sorted(off)))

    # 3 C++ == Python on every frame
    for w in C.WALKS:
        lines = C.cpp_lines(caps[w])
        py = [caw_read.line(i, s, s['flash']) for i, s in enumerate(states[w])]
        bad = [i for i in range(max(len(lines), len(py))) if i >= len(lines) or i >= len(py) or lines[i] != py[i]]
        C.report('3 C++ == Python %s' % w[:15], len(lines) == len(py) and not bad, '%d frames, %d lines differ%s' % (len(py), len(bad), (' (first at %d)' % bad[0]) if bad else ''))

    # 4 the past bug: the peeked words did not move, the drawn state did
    fx = json.load(open(FIXTURE, encoding='utf-8'))
    words_moved = sum(max(0, w['distinct_values'] - 1) for w in fx['words'])
    st = states[C.WALKS[0]]
    titles = {s['title'] for s in st}
    tabs = {s['tab'] for s in st if s['tab'] >= 0}
    hots = {s['hot'] for s in st if s['hot'] >= 0}
    ok = words_moved == 0 and len(titles) >= 3 and len(tabs) >= 2 and len(hots) >= 3
    C.report('4 past-bug fixture (counts from the summary, not a re-measure)', ok, 'Check B run %s: %d peeked words, %d value changes (fixture, counts only); the drawn state over the same walk: %d distinct titles (one blank), %d tab dots, %d hot rows'
             % (fx['run'], len(fx['words']), words_moved, len(titles), len(tabs), len(hots)))

    # 5 planted twins, in the Python reader and the shipped C++ reader
    cap = caps[C.WALKS[0]]
    tmp = os.path.join(C.REPO, 'out', 'n2', 'tmp_nosec69_selftest')
    os.makedirs(os.path.dirname(tmp), exist_ok=True)
    stripped = stripped_copy(cap, tmp)
    twins = [('white', caw_read.CawOpts(white=True), 'white'), ('ps_bank', caw_read.CawOpts(bank='ps'), 'ps_bank'), ('min_r_0.3', caw_read.CawOpts(min_r=0.3), 'min_r'),
             ('section_69_removed', caw_read.CawOpts(strip_runs=True), 'strip_runs')]
    for name, opts, plant in twins:
        py_probs = acceptance(caw_read.read_all(cap, opts))
        cpp_st = cpp_states_from_lines(C.cpp_lines(cap, plant))
        cpp_probs = acceptance(cpp_st)
        C.report('5 twin %s fails (py + C++)' % name, bool(py_probs) and bool(cpp_probs), 'python: %s | C++: %s' % ((py_probs or ['PASSED: the twin is not caught'])[0], (cpp_probs or ['PASSED: the twin is not caught'])[0]))
    # the section really removed from a frame file: "no text", never a guess, in both readers
    geom = open(os.path.join(stripped, 'geometry.bin'), 'rb').read()
    s0 = caw_read.read_cstate(os.path.join(stripped, 'frames', 'frame_000000.bin'), geom)
    cl = C.cpp_lines(stripped)
    c0 = cpp_states_from_lines(cl)[0]
    ok = (not s0['has_text'] and s0['title'] == '' and s0['rows'] == [] and s0['hot'] == -1 and not c0['has_text'] and c0['title'] == '' and c0['hot'] == -1 and len(cl) == 5)
    C.report('5 section 69 emptied in the file: "no text"', ok, 'python has_text=%s title=%r hot=%d | C++ has_text=%s title=%r hot=%d' % (s0['has_text'], s0['title'], s0['hot'], c0['has_text'], c0['title'], c0['hot']))
    shutil.rmtree(tmp, ignore_errors=True)
    # 5b a row moved 3 px off its slot: the page is not complete (review item 8), in both readers; the same frame unmoved is complete
    tmp2 = os.path.join(C.REPO, 'out', 'n2', 'tmp_shift_selftest')
    moved, hit = shifted_copy(cap, tmp2)
    still, _ = shifted_copy(cap, tmp2 + '_0', dy=0.0)
    pm = caw_read.read_cstate(os.path.join(moved, 'frames', 'frame_000000.bin'), open(os.path.join(moved, 'geometry.bin'), 'rb').read())
    p0 = caw_read.read_cstate(os.path.join(still, 'frames', 'frame_000000.bin'), open(os.path.join(still, 'geometry.bin'), 'rb').read())
    cm, c0 = cpp_states_from_lines(C.cpp_lines(moved))[0], cpp_states_from_lines(C.cpp_lines(still))[0]
    ok = (not pm['complete'] and not cm['complete'] and p0['complete'] and c0['complete'] and hit >= 1)
    C.report('5 twin: one row moved +3 px is not complete (py + C++)', ok, 'moved: python complete=%s C++ complete=%s; unmoved control: %s / %s (%d glyph draws moved)' % (pm['complete'], cm['complete'], p0['complete'], c0['complete'], hit))
    shutil.rmtree(tmp2, ignore_errors=True)
    shutil.rmtree(tmp2 + '_0', ignore_errors=True)
    unit = subprocess.run([C.test_exe(), '--unit'], capture_output=True, text=True).stdout.strip()
    py_unit = 'dim_red=%d grey=%d amber=%d white=%d red=%d pulse_floor=%d' % tuple(int(caw_read.is_hot(c)) for c in ((0.12, 0, 0), (0.392,) * 3, (0.941, 0.627, 0), (1, 1, 1), (0.92, 0.04, 0.02), (0.165, 0.03, 0.02)))
    C.report('5 a dim red (r 0.12, g = b = 0) is still hot; grey, amber, white are not', unit == py_unit == 'dim_red=1 grey=0 amber=0 white=0 red=1 pulse_floor=1', 'C++: %s | python: %s' % (unit, py_unit))
    # and the shipped readers on the unmodified walk pass the very same acceptance (the twins fail it, the real reader does not)
    ok_cpp = not acceptance(cpp_states_from_lines(C.cpp_lines(cap)))
    C.report('5 control: the shipped C++ reader passes the acceptance', ok_cpp, 'same acceptance function as the twins')
    ok = all(C.results)
    print('\n%s: %d checks' % ('PASS' if ok else 'FAIL', len(C.results)))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
