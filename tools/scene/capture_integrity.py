r"""Capture integrity lint (S103 lane IXF; the past bug: docs\research\s103\IX_index_overrun.md).

  py -I tools\scene\capture_integrity.py [CAPTURE ...] [--root OUT\CAPTURES] [--known] [--strict] [--frames] [--selftest]

For each capture folder (images.bin, geometry.bin, frames\frame_NNNNNN.bin) every draw that is not inline and not thin must have its vertex
range and its index range [offset, offset + bytes) INSIDE geometry.bin, and one offset must keep one size (the same offset with another
size is a conflict: the binder's geometry cache is keyed by offset). A frame with a range past the end of geometry.bin is BAD: its offsets name another capture's store (frame 0 of a second
capture in one process before the reader fix). Ranges of bad frames are left out of the overlap pass (they are not evidence of the real layout);
a frame that introduces a size conflict with an earlier, trusted range is BAD too.

Prints one line per capture and, for a bad one, one line per bad frame. With no CAPTURE, every folder under the root.
  --known   the sealed captures that carry a bad frame 0 (tools\scene\capture_integrity_known.txt) are expected: fail only when a capture has
            bad frames the list does not state (exact frame list and ref count), or a listed capture no longer shows them.
  --strict  fail on any bad frame (the default without --known).
  --frames  print only 'NAME: f,f,f' lines (machine form; tools\gate\corpus.py and the choice scoring use BadFrames() directly).
  --selftest  builds planted captures in a temp folder (a clean one, one with a stale frame 0, one with an overlap) and checks the verdicts.
Reads the player's captures locally; writes nothing into the repo.
"""
import glob
import os
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
KNOWN = os.path.join(HERE, 'capture_integrity_known.txt')
DRAW = struct.Struct('<QQQIIIIIIIIQQIIIIBBB5x')  # 96 bytes; [11] vertex offset, [12] index offset, [13] vertex bytes, [14] index bytes, [19] flags
MAGIC = 0x52464352
SEC_DRAWS = 4
SKIP_FLAGS = 17  # inline (1) or thin (16): per-frame bytes, never in the geometry store


def frame_refs(path):
    """Returns (capture_frame_index, [(offset, bytes, draw_index, kind)]) for one frame file, or None when it is not a frame."""
    b = open(path, 'rb').read()
    if len(b) < 32:
        return None
    magic, _ver, cf, sc, _gf, _pi, _fl = struct.unpack_from('<IIIIQII', b, 0)
    if magic != MAGIC:
        return None
    refs = []
    for s in range(sc):
        sid, es, cnt, off = struct.unpack_from('<IIQQ', b, 32 + 24 * s)
        if sid != SEC_DRAWS or es != DRAW.size:
            continue
        for i in range(cnt):
            d = DRAW.unpack_from(b, off + DRAW.size * i)
            if d[19] & SKIP_FLAGS:
                continue
            for o, n, kind in ((d[11], d[13], 'v'), (d[12], d[14], 'i')):
                if n and n != 0xFFFFFFFF:
                    refs.append((o, n, i, kind))
    return cf, refs


def check(folder):
    """Returns (bad, nframes) where bad = {frame: {'past_end': n, 'overlap': n, 'first': text}} for one capture folder."""
    gsize = os.path.getsize(os.path.join(folder, 'geometry.bin'))
    files = sorted(glob.glob(os.path.join(folder, 'frames', 'frame_*.bin')))
    frames = []
    for p in files:
        r = frame_refs(p)
        if r:
            frames.append(r)
    bad = {}
    for cf, refs in frames:
        pe = [(o, n, i, k) for (o, n, i, k) in refs if o + n > gsize]
        if pe:
            o, n, i, k = pe[0]
            bad[cf] = {'past_end': len(pe), 'overlap': 0, 'first': 'draw %d %s range %d+%d past the end %d' % (i, k, o, n, gsize)}
    # same-offset pass over the trusted frames, in frame order: one offset must keep one size (the binder's cache is keyed by offset; the reader's
    # store hands out one range per content key). A different size at an offset an earlier frame used flags the later frame. (Sub-ranges that
    # merely lie inside other ranges are legitimate: shared vertex streams are stored per range.)
    seen = {}  # offset -> (bytes, frame, draw, kind)
    for cf, refs in frames:
        if cf in bad:
            continue
        for (o, n, i, k) in refs:
            old = seen.setdefault(o, (n, cf, i, k))
            if old[0] != n:
                rec = bad.setdefault(cf, {'past_end': 0, 'overlap': 0, 'first': 'draw %d %s range %d+%d: offset %d was %d bytes in frame %d draw %d' % (i, k, o, n, o, old[0], old[1], old[2])})
                rec['overlap'] += 1
    return bad, len(frames)


def BadFrames(folder):
    """For tools\\gate\\corpus.py and the choice scoring: the sorted capture frame indices the lint flags, and the per-frame detail."""
    bad, _n = check(folder)
    return sorted(bad), bad


def captures(root, names):
    if names:
        return [os.path.join(root, n) for n in names]
    return sorted(p for p in glob.glob(os.path.join(root, '*')) if os.path.isfile(os.path.join(p, 'geometry.bin')) and os.path.isdir(os.path.join(p, 'frames')))


def load_known():
    """name -> {frame: past_end + overlap ref count}; lines: NAME FRAME REFS."""
    known = {}
    if os.path.isfile(KNOWN):
        for line in open(KNOWN, encoding='utf-8'):
            line = line.split('#')[0].strip()
            if line:
                name, f, n = line.split()
                known.setdefault(name, {})[int(f)] = int(n)
    return known


def run(root, names, known_mode, frames_only):
    known = load_known() if known_mode else {}
    rc = 0
    total_bad = 0
    for folder in captures(root, names):
        name = os.path.basename(folder)
        if not os.path.isfile(os.path.join(folder, 'geometry.bin')):
            print('%s: no geometry.bin (skipped)' % name)
            continue
        bad, n = check(folder)
        got = {f: r['past_end'] + r['overlap'] for f, r in bad.items()}
        if frames_only:
            if bad:
                print('%s: %s' % (name, ','.join(str(f) for f in sorted(bad))))
            continue
        want = known.get(name, {}) if known_mode else {}
        status = 'clean' if not bad else 'BAD'
        if known_mode and bad:
            status = 'known' if got == want else 'UNEXPECTED (expected %s)' % (sorted(want.items()) or 'clean')
        elif known_mode and want:
            status = 'FAIL: listed as bad (%s) but now clean' % sorted(want.items())
        print('%s: %d frames, %s' % (name, n, status))
        for f in sorted(bad):
            r = bad[f]
            print('    frame %d: %d refs past the end, %d size conflicts; %s' % (f, r['past_end'], r['overlap'], r['first']))
        total_bad += len(bad)
        if known_mode:
            if got != want:
                rc = 1
        elif bad:
            rc = 1
    if not frames_only:
        print('capture integrity: %d bad frame(s)%s' % (total_bad, ' (known set: %d capture(s) listed)' % len(known) if known_mode else ''))
    return rc


# ---- selftest ---------------------------------------------------------------------------------------------------------------------------

def _frame_bytes(cf, draws):
    """draws = [(voff, vbytes, ioff, ibytes, flags)]"""
    body = b''.join(DRAW.pack(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, v, io, vb, ib, 0, 0, 0, 0, fl) for (v, vb, io, ib, fl) in draws)
    off = 32 + 24
    head = struct.pack('<IIIIQII', MAGIC, 1, cf, 1, cf, 0, 0) + struct.pack('<IIQQ', SEC_DRAWS, DRAW.size, len(draws), off)
    return head + body


def _make(folder, gsize, frames):
    os.makedirs(os.path.join(folder, 'frames'))
    open(os.path.join(folder, 'geometry.bin'), 'wb').write(b'\0' * gsize)
    open(os.path.join(folder, 'images.bin'), 'wb').write(b'')
    for cf, draws in enumerate(frames):
        open(os.path.join(folder, 'frames', 'frame_%06d.bin' % cf), 'wb').write(_frame_bytes(cf, draws))


def selftest():
    fails = 0

    def expect(ok, what):
        nonlocal fails
        print('%s %s' % ('PASS' if ok else 'FAIL', what))
        fails += 0 if ok else 1

    with tempfile.TemporaryDirectory() as tmp:
        good = [[(0, 100, 100, 50, 0)], [(0, 100, 100, 50, 0), (200, 64, 264, 32, 0)], [(0, 100, 100, 50, 0)]]
        _make(os.path.join(tmp, 'good'), 400, good)
        # a stale frame 0: its offsets point past this capture's geometry.bin (the a30_match shape), later frames are real
        stale = [[(5000, 260, 5260, 40, 0)]] + good[1:]
        _make(os.path.join(tmp, 'stale0'), 400, stale)
        # an overlap: frame 2 stores a different-size range at an offset frame 1 already used
        over = good[:2] + [[(200, 128, 400, 8, 0)]]
        _make(os.path.join(tmp, 'overlap'), 600, over)
        # inline / thin draws never count (flags 1, 16) even with ranges past the end
        inline = [[(9999, 10, 9999, 10, 1)]] + good[1:]
        _make(os.path.join(tmp, 'inline'), 400, inline)
        frames, _ = BadFrames(os.path.join(tmp, 'good'))
        expect(frames == [], 'a clean capture has no bad frame')
        frames, det = BadFrames(os.path.join(tmp, 'stale0'))
        expect(frames == [0] and det[0]['past_end'] == 2, 'a stale frame 0 (offsets past the end of geometry.bin) is found: frame 0, 2 refs')
        frames, _ = BadFrames(os.path.join(tmp, 'overlap'))
        expect(frames == [2], 'a different-size range at an offset an earlier frame used is found: frame 2')
        frames, _ = BadFrames(os.path.join(tmp, 'inline'))
        expect(frames == [], 'inline draws are not checked')
        rc = run(tmp, ['stale0'], False, False)
        expect(rc == 1, 'strict mode exits 1 on the planted capture')
        rc = run(tmp, ['good'], False, False)
        expect(rc == 0, 'strict mode exits 0 on the clean capture')
    print('FAILED' if fails else 'all passed')
    return 1 if fails else 0


def main(argv):
    args = [a for a in argv if not a.startswith('--')]
    if '--root' in argv:  # the gate lints' convention (the repo root): not used here
        args = [a for a in args if a != argv[argv.index('--root') + 1]]
    root = os.path.join(os.environ.get('RDR_MAIN_CHECKOUT', os.path.dirname(os.path.dirname(HERE))), 'out', 'captures')
    if '--captures-root' in argv:
        k = argv.index('--captures-root')
        root = argv[k + 1]
        args = [a for a in args if a != root]
    if '--selftest' in argv:
        return selftest()
    return run(root, args, '--known' in argv, '--frames' in argv)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
