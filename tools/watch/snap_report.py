"""Report on `snap` recordings (src/guest_watch.h): which bytes of a guest range change around a script mark.

    py tools/watch/snap_report.py RUN                      ranges, marks, and every word that changed in the run
    py tools/watch/snap_report.py RUN --mark ID [--window N] [--name NAME] [--all-frames] [--json]
        before (mark frame - N) vs after (mark frame + N), per word: which bytes changed, values as u32 / s32 /
        float, how many frames inside the window changed it; plus per-frame changed-byte counts in the window

RUN is the run number (212 or 212 padded), a folder name under out/harness/watch (e.g. win-amd64-coverage_012) or a
path to the folder. Marks come from <folder>/marks.tsv (written only while a watch/snap runs) merged with the
run's log (out/build/<exe folder>/logs/wwe2k14_RUN.log, "[wwe2k14][input] mark ID TEXT (frame N)"), so marks
issued before the first watch/snap or after unsnap are found too.

A LEN that is not a multiple of 4 is reported in whole words with the last word zero-padded past LEN (padding
bytes never count as changes; the byte mask shows them as '.').

Everything printed is game-derived memory: keep it out of docs/ and db/ beyond addresses and one-line summaries.
"""
import argparse
import json
import pathlib
import re
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
WATCH = ROOT / 'out' / 'harness' / 'watch'
HEADER = 64


def find_dir(run):
    p = pathlib.Path(run)
    if p.is_dir():
        return p
    for name in (run, run.zfill(3) if run.isdigit() else run):
        if (WATCH / name).is_dir():
            return WATCH / name
    sys.exit('snap_report: no folder %s under %s' % (run, WATCH))


def load_range(path):
    data = path.read_bytes()
    if len(data) < HEADER or data[:8] != b'W2KSNAP1':
        raise ValueError('%s: not a snap file' % path)
    guest, length, header = struct.unpack_from('<III', data, 8)
    name = data[20:header].split(b'\0', 1)[0].decode('ascii', 'replace')
    records = []  # (frame, bytes or None); unchanged frames carry the previous bytes object
    pad = b'\0' * (-length % 4)  # whole words: the last one zero-padded past LEN
    pos, prev = header, None
    while pos + 9 <= len(data):
        frame, status = struct.unpack_from('<QB', data, pos)
        pos += 9
        if status == 1:
            if pos + length > len(data):
                break  # torn last record (file still being written or the process ended mid-write)
            prev = data[pos:pos + length] + pad
            pos += length
            records.append((frame, prev))
        elif status == 2:
            records.append((frame, prev))
        else:
            prev = None
            records.append((frame, None))
    return {'name': name, 'guest': guest, 'len': length, 'records': records, 'file': str(path)}


def load_marks(folder):
    """Marks from marks.tsv and from the run's log, merged (one per (id, frame)), in frame order."""
    marks, sources = {}, []
    tsv = folder / 'marks.tsv'
    if tsv.is_file():
        for line in tsv.read_text(encoding='utf-8', errors='replace').splitlines()[1:]:
            parts = line.split('\t')
            if len(parts) >= 2 and parts[0].isdigit() and parts[1].isdigit():
                marks.setdefault((int(parts[0]), int(parts[1])), parts[2] if len(parts) > 2 else '')
        sources.append(str(tsv))
    m = re.match(r'(?:(.+)_)?(\d+)$', folder.name)
    if m:
        exe_folder = m.group(1) or 'win-amd64-release'
        log = ROOT / 'out' / 'build' / exe_folder / 'logs' / ('wwe2k14_%s.log' % m.group(2))
        if log.is_file():
            pat = re.compile(r'\[wwe2k14\]\[input\] mark (\d+)(?: (.*))? \(frame (\d+)\)')
            for line in log.read_text(encoding='utf-8', errors='replace').splitlines():
                hit = pat.search(line)
                if hit:
                    marks.setdefault((int(hit.group(1)), int(hit.group(3))), hit.group(2) or '')
            sources.append(str(log))
    merged = sorted(((i, f, t) for (i, f), t in marks.items()), key=lambda x: (x[1], x[0]))
    return merged, ' + '.join(sources) or None


def state_at(records, frame):
    """Bytes of the latest record at or before frame (None if unreadable or before the first record)."""
    lo, hi, best = 0, len(records) - 1, None
    while lo <= hi:
        mid = (lo + hi) // 2
        if records[mid][0] <= frame:
            best = mid
            lo = mid + 1
        else:
            hi = mid - 1
    return (records[best][0], records[best][1]) if best is not None else (None, None)


def word_values(b):
    u = struct.unpack('>I', b)[0]
    s = struct.unpack('>i', b)[0]
    f = struct.unpack('>f', b)[0]
    ftxt = ('%.6g' % f) if f == f and abs(f) < 1e12 and (f == 0 or abs(f) > 1e-12) else '-'
    return u, s, ftxt


def changes_by_word(records, start, end, length):
    """Per word offset: frames in (start, end] whose record differs from the previous one in that word."""
    counts, first = {}, {}
    per_frame = []
    prev = None
    for frame, b in records:
        if frame > end:
            break
        if frame <= start or prev is None or b is None:
            prev = b  # the state at the window start (or after an unreadable frame)
            continue
        if b is not prev and b != prev:
            nbytes = 0
            for off in range(0, length, 4):  # (records are padded to whole words)
                if b[off:off + 4] != prev[off:off + 4]:
                    counts[off] = counts.get(off, 0) + 1
                    first.setdefault(off, frame)
                    nbytes += sum(1 for i in range(off, min(off + 4, length)) if b[i] != prev[i])
            per_frame.append((frame, nbytes))
        prev = b
    return counts, first, per_frame


def mask(before, after, off, length):
    return ''.join('X' if i < length and before[i] != after[i] else '.' for i in range(off, off + 4))


def report_run(r, out):
    rec = r['records']
    readable = [x for x in rec if x[1] is not None]
    out.append('range %s 0x%08X..0x%08X (%d bytes): %d frames recorded (%s..%s), %d unreadable' % (
        r['name'], r['guest'], r['guest'] + r['len'] - 1, r['len'], len(rec), rec[0][0] if rec else '-',
        rec[-1][0] if rec else '-', len(rec) - len(readable)))
    if not readable:
        return
    counts, first, per_frame = changes_by_word(rec, rec[0][0] - 1, rec[-1][0], r['len'])
    out.append('  %d frames with changed bytes; words that changed (offset, address, changes, first/last value):' %
               len(per_frame))
    first_b, last_b = readable[0][1], readable[-1][1]
    for off in sorted(counts):
        u0, _, f0 = word_values(first_b[off:off + 4])
        u1, _, f1 = word_values(last_b[off:off + 4])
        out.append('    +0x%03X 0x%08X  %6d changes (first at frame %d)  0x%08X (%s) .. 0x%08X (%s)' % (
            off, r['guest'] + off, counts[off], first[off], u0, f0, u1, f1))


def report_mark(r, mark_frame, window, all_frames, out, js):
    rec = r['records']
    before_frame, before = state_at(rec, mark_frame - window)
    after_frame, after = state_at(rec, mark_frame + window)
    out.append('range %s 0x%08X (%d bytes): before = frame %s, after = frame %s (mark at %d, window %d)' % (
        r['name'], r['guest'], r['len'], before_frame, after_frame, mark_frame, window))
    entry = {'name': r['name'], 'guest': r['guest'], 'len': r['len'], 'before_frame': before_frame,
             'after_frame': after_frame, 'words': [], 'per_frame': []}
    js.append(entry)
    if before is None or after is None:
        out.append('  no readable snapshot at one end of the window')
        return
    counts, first, per_frame = changes_by_word(rec, mark_frame - window, mark_frame + window, r['len'])
    entry['per_frame'] = per_frame
    rows = []
    for off in range(0, r['len'], 4):  # the last word may be zero-padded past LEN
        b, a = before[off:off + 4], after[off:off + 4]
        if b != a or counts.get(off):
            ub, sb, fb = word_values(b)
            ua, sa, fa = word_values(a)
            rows.append((off, b != a))
            entry['words'].append({'offset': off, 'address': r['guest'] + off, 'before': ub, 'after': ua,
                                   'changed_frames': counts.get(off, 0), 'first_change': first.get(off)})
            out.append('  +0x%03X 0x%08X %s  0x%08X (%d, %s) -> 0x%08X (%d, %s)  changed on %d frame(s) in the '
                       'window%s' % (off, r['guest'] + off, mask(before, after, off, r['len']), ub, sb, fb, ua, sa,
                                     fa, counts.get(off, 0),
                                     ', first %d' % first[off] if off in first else ''))
    if not rows:
        out.append('  no byte differs and nothing changed inside the window')
    nb = sum(1 for _, d in rows if d)
    out.append('  %d word(s) differ before vs after; %d word(s) changed somewhere in the window' % (
        nb, len(rows)))
    if per_frame:
        shown = per_frame if all_frames else per_frame[:40]
        out.append('  per-frame changed bytes (frame:bytes, relative to the mark): ' + ' '.join(
            '%d:%d' % (f - mark_frame, n) for f, n in shown) + (
            '' if len(shown) == len(per_frame) else ' ... (%d more; --all-frames)' % (len(per_frame) - len(shown))))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('run')
    ap.add_argument('--mark', type=int, help='script mark ID')
    ap.add_argument('--window', type=int, default=60, help='frames before/after the mark (default 60)')
    ap.add_argument('--name', help='only this range')
    ap.add_argument('--all-frames', action='store_true', help='list every changed frame in the window')
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args()
    folder = find_dir(a.run)
    ranges = []
    for p in sorted(folder.glob('snap_*.bin')):
        r = load_range(p)
        if not a.name or r['name'] == a.name:
            ranges.append(r)
    if not ranges:
        sys.exit('snap_report: no snap_*.bin%s in %s' % (' named ' + a.name if a.name else '', folder))
    marks, marks_src = load_marks(folder)
    out, js = ['snap folder %s' % folder], []
    out.append('marks (%s): %s' % (marks_src or 'none found',
                                   ', '.join('%d "%s" frame %d' % (i, t, f) for i, f, t in marks) or '-'))
    if a.mark is None:
        for r in ranges:
            report_run(r, out)
    else:
        hit = [m for m in marks if m[0] == a.mark]
        if not hit:
            sys.exit('snap_report: no mark %d (marks: %s)' % (a.mark, ', '.join(str(m[0]) for m in marks) or '-'))
        mark_frame = hit[0][1]
        out.append('mark %d "%s" at frame %d%s' % (a.mark, hit[0][2], mark_frame,
                                                    '' if len(hit) == 1 else ' (first of %d with this ID)' % len(hit)))
        for r in ranges:
            report_mark(r, mark_frame, a.window, a.all_frames, out, js)
    if a.json:
        print(json.dumps({'folder': str(folder), 'marks': marks, 'ranges': js}, indent=1))
    else:
        print('\n'.join(out))


if __name__ == '__main__':
    main()
