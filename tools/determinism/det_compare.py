"""Compare two runs recorded with --wwe2k14_det_hash (Session 8, task 8b). Read-only; never launches the game.

    py tools/determinism/det_compare.py A B [--slack N] [--exe-dir PRESET_OR_DIR] [--out FILE] [--bytes N] [--no-shots]

A and B are run numbers (wwe2k14_NNN.log). Reads
  out/harness/determinism/det_NNN.csv   per-frame region hashes (src/frame_hash.h); det_<exe folder>_NNN.csv for an
                                        exe folder other than win-amd64-release (--exe-dir)
  out/harness/determinism/det_NNN.bin   masked region bytes every N frames (--wwe2k14_det_hash_raw_every)
  out/build/<exe dir>/logs/wwe2k14_NNN.log   input-script events "(frame N)", "saw" lines and [file] opens
  out/build/<exe dir>/captures/NNN_*.bmp      screenshots (compared like tools/bench/shot_diff.py)
and prints:
  0. Build identity of each CSV (exe path, size, mtime; written by frame_hash.h since the review) and a warning when
     they differ.
  1. Input-script events with each run's frame and the difference. Alignment points are the logged events plus the
     frame each wait_file / press_until_file step saw its file (that is where a load-time offset changes):
     wait_file = its start frame + N from "saw 'x' after N frames"; press_until_file = the frame of the matching
     [file] open + 1 (the script checks opens at the next frame).
  2. File-open order: the first open whose path differs, and the first open at a different frame.
  3. Per region: first differing frame, (a) frame against frame and (b) aligned to the latest alignment point
     (B's frame = A's frame + that point's delta, +-slack), the share of equal frames among the frames readable in
     both runs, and how many frames were unreadable on both / one side.
  3b. The equal share per segment between alignment points.
  3c. Per region: the A frame ranges where the aligned compare differs.
  4. Bytes: at the first aligned raw dump pair that differs, which byte offsets differ, and over all differing
     aligned dump pairs the most frequent differing words (offset, number of pairs).
  5. Screenshots: MAD / max / share of pixels off by more than 16, and whether the pixels are identical.
The text also goes to out/harness/determinism/compare_A_B.txt (compare_A_B_slackN.txt with --slack N; game-derived
data: stays under out/).
"""
import argparse
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
DET = os.path.join(REPO, 'out', 'harness', 'determinism')
BUILD = os.path.join(REPO, 'out', 'build')
EXE_DIR = os.path.join(BUILD, 'win-amd64-release')

EVENT_RE = re.compile(r"\[wwe2k14\]\[input\] line (\d+)(?: \([^)]*\))?: (.*) \(frame (\d+)\)\s*$")
WAIT_RE = re.compile(r"\[wwe2k14\]\[input\] line (\d+)(?: \([^)]*\))?: waiting for a file containing '([^']*)' \(frame (\d+)\)")
SAW_RE = re.compile(r"\[wwe2k14\]\[input\] line (\d+)(?: \([^)]*\))?: saw '([^']*)' after (\d+) frames")
PSAW_RE = re.compile(r"\[wwe2k14\]\[input\] line (\d+)(?: \([^)]*\))?: saw '([^']*)' (\d+) frames after press (\d+)")
FRAME_TAIL_RE = re.compile(r"\(frame (\d+)\)\s*$")
FILE_RE = re.compile(r"\[wwe2k14\]\[file\] \S+ '([^']*)' .* frame=(\d+)")
HASH_RE = re.compile(r'^(?:[0-9a-f]{16}|-)$')


def fail(msg):
    sys.exit('det_compare: ' + msg)


def det_stem(run, exe_dir):
    folder = os.path.basename(os.path.normpath(exe_dir))
    if folder == 'win-amd64-release':
        return 'det_%03d' % run
    return 'det_%s_%03d' % (folder, run)


def read_log(run, exe_dir=None):
    """Returns (events, files). events: [(label, frame)] in log order, including the 'saw' alignment points."""
    exe_dir = exe_dir or EXE_DIR
    path = os.path.join(exe_dir, 'logs', 'wwe2k14_%03d.log' % run)
    if not os.path.exists(path):
        fail('no log for run %d: %s' % (run, path))
    events, files, wait_start = [], [], {}
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = SAW_RE.search(line)
            if m:
                ln, text, n = m.group(1), m.group(2), int(m.group(3))
                tail = FRAME_TAIL_RE.search(line)
                if tail:
                    events.append(('line %s: saw %s' % (ln, text), int(tail.group(1))))
                elif ln in wait_start:
                    events.append(('line %s: saw %s' % (ln, text), wait_start[ln] + n))
                continue
            m = PSAW_RE.search(line)
            if m:
                ln, text = m.group(1), m.group(2)
                tail = FRAME_TAIL_RE.search(line)
                if tail:
                    events.append(('line %s: saw %s (press)' % (ln, text), int(tail.group(1))))
                else:
                    opened = [fr for p, fr in reversed(files) if text in p]
                    if opened:
                        events.append(('line %s: saw %s (press)' % (ln, text), opened[0] + 1))
                continue
            m = EVENT_RE.search(line)
            if m:
                text = m.group(2)
                w = WAIT_RE.search(line)
                if w:
                    wait_start[w.group(1)] = int(w.group(3))
                if text.startswith('waiting for') or text.startswith('screenshot') or 'scene=' in text \
                        or text.startswith('bench:') or text.startswith('pressing'):
                    events.append(('line %s: %s' % (m.group(1), text), int(m.group(3))))
                continue
            m = FILE_RE.search(line)
            if m:
                files.append((m.group(1).lower(), int(m.group(2))))
    return events, files


def read_csv(run, exe_dir=None):
    """Returns (names, rows, meta, dropped). rows: {frame: [hash or '-']}; meta: the '# det_hash ...' line or ''."""
    path = os.path.join(DET, det_stem(run, exe_dir or EXE_DIR) + '.csv')
    if not os.path.exists(path):
        fail('no hash CSV for run %d: %s (was the run made with --wwe2k14_det_hash, and with this --exe-dir?)'
             % (run, path))
    meta, header, rows, dropped = '', None, {}, 0
    with open(path, encoding='utf-8') as f:
        for line in f:
            line = line.rstrip('\r\n')
            if line.startswith('#'):
                meta = meta or line[1:].strip()
                continue
            if header is None:
                header = line.split(',')
                continue
            parts = line.split(',')
            if len(parts) != len(header) or not parts[0].isdigit() or not all(HASH_RE.match(p) for p in parts[1:]):
                dropped += 1  # e.g. the last line, cut off when the process quit
                continue
            rows[int(parts[0])] = parts[1:]
    if header is None:
        fail('%s has no header line' % path)
    return header[1:], rows, meta, dropped


def read_bin(run, exe_dir=None):
    path = os.path.join(DET, det_stem(run, exe_dir or EXE_DIR) + '.bin')
    out = {}
    if not os.path.exists(path):
        return out
    with open(path, 'rb') as f:
        data = memoryview(f.read())
    pos = 0
    while pos + 16 <= len(data):
        if bytes(data[pos:pos + 4]) != b'DETR':
            break
        frame, count = struct.unpack_from('<QI', data, pos + 4)
        pos += 16
        regions = []
        ok = True
        for _ in range(count):
            if pos + 4 > len(data):
                ok = False
                break
            (n,) = struct.unpack_from('<I', data, pos)
            pos += 4
            if pos + n > len(data):
                ok = False
                break
            regions.append(data[pos:pos + n] if n else None)
            pos += n
        if not ok:
            break
        out[frame] = regions
    return out


def byte_ranges(a, b, limit):
    a, b = bytes(a), bytes(b)
    ranges, i, n = [], 0, min(len(a), len(b))
    while i < n:
        if a[i] != b[i]:
            j = i
            while j < n and a[j] != b[j]:
                j += 1
            ranges.append((i, j))
            i = j
        else:
            i += 1
    total = sum(e - s for s, e in ranges)
    shown = []
    for s, e in ranges[:limit]:
        ws, we = s & ~3, (e + 3) & ~3
        shown.append('+%d..+%d (%d B) A=%s B=%s' % (s, e - 1, e - s, a[ws:we][:16].hex(), b[ws:we][:16].hex()))
    return total, len(ranges), shown


def word_diffs(a, b):
    """4-byte-aligned word offsets where a and b differ."""
    n = min(len(a), len(b)) & ~3
    try:
        import numpy as np
        wa = np.frombuffer(a, dtype=np.uint32, count=n // 4)
        wb = np.frombuffer(b, dtype=np.uint32, count=n // 4)
        return [int(i) * 4 for i in np.nonzero(wa != wb)[0]]
    except ImportError:
        return [o for o in range(0, n, 4) if a[o:o + 4] != b[o:o + 4]]


def frame_ranges(frames):
    out = []
    for f in frames:
        if out and f == out[-1][1] + 1:
            out[-1][1] = f
        else:
            out.append([f, f])
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0], epilog=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('a', type=int)
    ap.add_argument('b', type=int)
    ap.add_argument('--out', default=None)
    ap.add_argument('--bytes', type=int, default=8, help='byte ranges to show per region')
    ap.add_argument('--no-shots', action='store_true')
    ap.add_argument('--exe-dir', default='win-amd64-release',
                    help='exe folder of both runs: a preset name under out/build or a path (default %(default)s)')
    ap.add_argument('--slack', type=int, default=0,
                    help='aligned compare: also accept B rows up to N frames either side of the aligned frame '
                         '(separates one-frame alignment errors at load-time changes from real divergence)')
    ap.add_argument('--file-anchors', action='store_true',
                    help='also use every file open (same path, same order in both runs) as an alignment point; '
                         'finer inside long loads, where the offset changes between script events')
    args = ap.parse_args()
    exe_dir = args.exe_dir if os.path.isabs(args.exe_dir) or os.sep in args.exe_dir or '/' in args.exe_dir \
        else os.path.join(BUILD, args.exe_dir)
    if not os.path.isdir(exe_dir):
        fail('exe folder not found: %s' % exe_dir)
    lines = []

    def say(s=''):
        lines.append(s)
        print(s)

    say('determinism compare: run %03d vs run %03d%s' % (args.a, args.b,
                                                         ' (aligned compare with +-%d frames of slack)' % args.slack
                                                         if args.slack else ''))
    ev_a, files_a = read_log(args.a, exe_dir)
    ev_b, files_b = read_log(args.b, exe_dir)
    names_a, rows_a, meta_a, drop_a = read_csv(args.a, exe_dir)
    names_b, rows_b, meta_b, drop_b = read_csv(args.b, exe_dir)
    if not rows_a or not rows_b:
        fail('no complete rows in one of the CSVs (A %d rows, B %d rows)' % (len(rows_a), len(rows_b)))

    say('')
    say('0. build identity')
    say('   A: %s' % (meta_a or 'not recorded (CSV written before the build identity line was added)'))
    say('   B: %s' % (meta_b or 'not recorded (CSV written before the build identity line was added)'))
    strip_spec = lambda m: re.sub(r'^det_hash ', '', m.split(' spec=')[0])
    if meta_a and meta_b and strip_spec(meta_a) != strip_spec(meta_b):
        say('   WARNING: the two runs used different exes; differences may come from the build, not the game')
    elif not (meta_a and meta_b):
        say('   (not checked: identity missing on at least one side)')
    else:
        say('   same exe (path, size, mtime)')
    if drop_a or drop_b:
        say('   dropped incomplete CSV lines: A %d, B %d' % (drop_a, drop_b))

    say('')
    say('1. input-script events and alignment points (frame A, frame B, B-A)')
    anchors = []  # (frame_a, delta, label)
    for (la, fa), (lb, fb) in zip(ev_a, ev_b):
        same_ev = '' if la == lb else '   <-- different event: %s' % lb
        say('   %-60s %6d %6d %+5d%s' % (la[:60], fa, fb, fb - fa, same_ev))
        if la == lb:
            anchors.append((fa, fb - fa, la))
    if len(ev_a) != len(ev_b):
        say('   event count differs: %d vs %d' % (len(ev_a), len(ev_b)))
    anchors.sort(key=lambda x: x[0])

    say('')
    say('2. file opens: %d in A, %d in B' % (len(files_a), len(files_b)))
    first_path_diff = None
    first_frame_diff = None
    same_frame = 0
    for i, ((pa, fa), (pb, fb)) in enumerate(zip(files_a, files_b)):
        if pa != pb:
            first_path_diff = (i, pa, fa, pb, fb)
            break
        if fa == fb:
            same_frame += 1
        elif first_frame_diff is None:
            first_frame_diff = (i, pa, fa, fb)
    if first_frame_diff:
        say('   first open at a different frame: #%d %s, A frame %d, B frame %d' % first_frame_diff)
    if first_path_diff:
        i, pa, fa, pb, fb = first_path_diff
        say('   first different path at open #%d: A %s (frame %d) vs B %s (frame %d)' % (i, pa, fa, pb, fb))
    else:
        say('   same path order for all %d common opens' % min(len(files_a), len(files_b)))
    say('   opens with the same path at the same frame: %d of %d' % (same_frame, first_path_diff[0] if first_path_diff
                                                                     else min(len(files_a), len(files_b))))
    if args.file_anchors:
        n_common = first_path_diff[0] if first_path_diff else min(len(files_a), len(files_b))
        for i in range(n_common):
            (pa, fa), (_, fb) = files_a[i], files_b[i]
            anchors.append((fa, fb - fa, 'open #%d %s' % (i, os.path.basename(pa.replace(chr(92), '/')))))
        anchors.sort(key=lambda x: x[0])
        say('   --file-anchors: %d file opens added as alignment points (section 3b lists the segments)' % n_common)

    if names_a != names_b:
        fail('region lists differ: %s vs %s' % (names_a, names_b))
    names = names_a

    def delta_at(frame):
        d, label, start = 0, 'boot', 0
        for fa, dd, lab in anchors:
            if fa <= frame:
                d, label, start = dd, lab, fa
            else:
                break
        return d, label, start

    delta_cache = {f: delta_at(f) for f in rows_a}

    def same(f, k, d):
        for s in range(-args.slack, args.slack + 1):
            g = f + d + s
            if g in rows_b and rows_a[f][k] == rows_b[g][k]:
                return True
        return False

    say('')
    say('3. per region: first differing frame (A numbering)%s' %
        (' (aligned compare accepts +-%d frames of slack)' % args.slack if args.slack else ''))
    say('   equal share = equal frames / frames readable in both; "unread" = frames unreadable in both / in one run')
    say('   %-6s %-26s %-50s %-14s %s' % ('region', 'frame vs frame (equal)', 'aligned to latest point (equal)',
                                          'unread both/1', 'state'))
    common = sorted(set(rows_a) & set(rows_b))
    first_aligned = {}
    diff_frames = {}
    for k, name in enumerate(names):
        first_abs, eq_abs, rd_abs = None, 0, 0
        for f in common:
            va, vb = rows_a[f][k], rows_b[f][k]
            if va == '-' and vb == '-':
                continue
            rd_abs += 1
            if va == vb:
                eq_abs += 1
            elif first_abs is None:
                first_abs = f
        first_al, eq_al, rd_al, un_both, un_one, seg = None, 0, 0, 0, 0, ''
        diffs = []
        for f in sorted(rows_a):
            d, label, _ = delta_cache[f]
            g = f + d
            if g not in rows_b:
                continue
            va, vb = rows_a[f][k], rows_b[g][k]
            if va == '-' and vb == '-':
                un_both += 1
                continue
            if same(f, k, d):
                if va == '-' or vb == '-':
                    un_one += 1  # matched a slack neighbour, but the exact pair has one side unreadable
                else:
                    rd_al += 1
                    eq_al += 1
                continue
            if va == '-' or vb == '-':
                un_one += 1
            else:
                rd_al += 1
            diffs.append(f)
            if first_al is None:
                first_al, seg = f, '%s %+d' % (label[:30], d)
        diff_frames[name] = diffs
        first_aligned[name] = (first_al, delta_cache[first_al][0] if first_al is not None else 0)
        readable = sum(1 for f in common if rows_a[f][k] != '-')
        frames_a = sorted(rows_a)
        changes = sum(1 for p, q in zip(frames_a, frames_a[1:]) if rows_a[p][k] != rows_a[q][k])
        state = 'unreadable in all frames' if readable == 0 else ('identical' if first_al is None else 'diverges')
        state += ', changes %d times in A' % changes
        abs_txt = '%s (%.1f%%)' % ('none' if first_abs is None else first_abs, 100.0 * eq_abs / max(1, rd_abs))
        al_txt = '%s (%.1f%%)%s' % ('none' if first_al is None else first_al, 100.0 * eq_al / max(1, rd_al),
                                   (' in ' + seg) if seg else '')
        say('   %-6s %-26s %-50s %-14s %s' % (name, abs_txt, al_txt[:50], '%d/%d' % (un_both, un_one), state))

    say('')
    say('3b. equal share (%, aligned) per segment between alignment points; segment = frames from that point on')
    seg_starts = [(0, 0, 'boot')] + anchors
    say('   %-30s %6s ' % ('segment (A frame)', 'frames') + ' '.join('%5s' % n[:5] for n in names))
    for si, (start, d, label) in enumerate(seg_starts):
        end = seg_starts[si + 1][0] if si + 1 < len(seg_starts) else max(rows_a) + 1
        frames = [f for f in range(start, end) if f in rows_a and f + d in rows_b]
        if not frames:
            continue
        cells = []
        for k in range(len(names)):
            eq = sum(1 for f in frames if same(f, k, d))
            cells.append('%5.0f' % (100.0 * eq / len(frames)) if eq != len(frames) else '  ===')
        short = re.sub(r"^line \d+(?: \([^)]*\))?: ", '', label).replace('waiting for a file containing ', 'wait ')
        say('   %-30s %6d ' % (('%d %s' % (start, short))[:30], len(frames)) + ' '.join(cells))
    say('   (=== all frames equal; in this table a frame unreadable in both runs counts as equal)')

    say('')
    say('3c. A frame ranges where the aligned compare differs (first 12 ranges per region)')
    for name in names:
        diffs = diff_frames[name]
        if not diffs:
            continue
        rs = frame_ranges(diffs)
        txt = ', '.join('%d-%d' % (s, e) if e > s else '%d' % s for s, e in rs[:12])
        say('   %-6s %d frames in %d ranges: %s%s' % (name, len(diffs), len(rs), txt, ' ...' if len(rs) > 12 else ''))

    raw_a, raw_b = read_bin(args.a, exe_dir), read_bin(args.b, exe_dir)
    say('')
    say('4. bytes at aligned raw dump pairs (%d / %d dumps); offsets are from the region start (after masking)' %
        (len(raw_a), len(raw_b)))
    for k, name in enumerate(names):
        f, d = first_aligned[name]
        if f is None:
            continue
        # Only an aligned pair is meaningful: A dump fa and B dump fa + offset. Dumps fall on absolute frames
        # (multiples of N), so a pair exists only while the B-A offset is a multiple of N.
        pairs = [x for x in diff_frames[name] if x in raw_a and x + delta_cache[x][0] in raw_b]
        if not pairs:
            any_pair = any(x + delta_cache[x][0] in raw_b for x in raw_a if x >= f)
            say('   %-6s no aligned dump pair on a differing frame at or after %d (%s)' %
                (name, f, 'pairs exist, but only on equal frames' if any_pair else
                 'B-A offset %+d is not a multiple of the dump interval' % d))
            continue
        fa = pairs[0]
        fb = fa + delta_cache[fa][0]
        a_bytes, b_bytes = raw_a[fa][k], raw_b[fb][k]
        if a_bytes is None or b_bytes is None:
            say('   %-6s A frame %d / B frame %d: unreadable on one side' % (name, fa, fb))
            continue
        total, nr, shown = byte_ranges(a_bytes, b_bytes, args.bytes)
        say('   %-6s A frame %d vs B frame %d: %d of %d bytes differ in %d ranges (%d differing frames have an '
            'aligned dump pair)' % (name, fa, fb, total, len(a_bytes), nr, len(pairs)))
        for s in shown:
            say('          ' + s)
        counts = {}
        for x in pairs[:3000]:
            ra, rb = raw_a[x][k], raw_b[x + delta_cache[x][0]][k]
            if ra is None or rb is None:
                continue
            for o in word_diffs(ra, rb):
                counts[o] = counts.get(o, 0) + 1
        top = sorted(counts.items(), key=lambda kv: (-kv[1], kv[0]))[:12]
        if top:
            say('          most frequent differing words over %d pairs: %s' %
                (min(len(pairs), 3000), ', '.join('+%d (%d)' % kv for kv in top)))

    if not args.no_shots:
        say('')
        say('5. screenshots (MAD over RGB 0-255, max, % of pixels off by >16, identical pixels)')
        try:
            import numpy as np
            from PIL import Image
            cap = os.path.join(exe_dir, 'captures')
            pa = '%03d_' % args.a  # Session 16: run numbers past 999 have 4 digits
            shot = {}  # label -> file (Session 17: PNG by default, BMP before)
            for fn in os.listdir(cap):
                if fn.startswith(pa) and fn.endswith(('.bmp', '.png')):
                    shot.setdefault(fn[len(pa):-4], set()).add(fn[-4:])
            labels = sorted(shot)

            def shot_file(run, label):
                for ext in ('.png', '.bmp'):
                    f = os.path.join(cap, '%03d_%s%s' % (run, label, ext))
                    if os.path.exists(f):
                        return f
                return os.path.join(cap, '%03d_%s.png' % (run, label))
            say('   %d screenshots in A' % len(labels))
            for label in labels:
                pb = shot_file(args.b, label)
                if not os.path.exists(pb):
                    say('   %-14s missing in B' % label)
                    continue
                with Image.open(shot_file(args.a, label)) as ia, Image.open(pb) as ib:
                    a = np.asarray(ia.convert('RGB'), dtype=np.int16)
                    b = np.asarray(ib.convert('RGB'), dtype=np.int16)
                if a.shape != b.shape:
                    say('   %-14s size differs' % label)
                    continue
                dd = np.abs(a - b)
                say('   %-14s MAD %6.3f  max %3d  off %6.2f%%  %s' % (label, float(dd.mean()), int(dd.max()),
                                                                     float((dd.max(axis=2) > 16).mean() * 100),
                                                                     'IDENTICAL' if int(dd.max()) == 0 else ''))
        except ImportError as e:
            say('   skipped (%s)' % e)

    suffix = '_slack%d' % args.slack if args.slack else ''
    out = args.out or os.path.join(DET, 'compare_%03d_%03d%s.txt' % (args.a, args.b, suffix))
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    print('\nwrote %s' % out)


if __name__ == '__main__':
    main()
