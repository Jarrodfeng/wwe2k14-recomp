"""S101 lane 31 (go-anywhere N5): moments from recordings. A scene a person can reach once is reachable on demand ever after.

    py -I tools/goto/moments.py list
    py -I tools/goto/moments.py add NAME RECFILE [--det CSV] [--shows "words"] [--build "words"]    register a recording Jarrod made
    py -I tools/goto/moments.py check [NAME]                       parse + validate the library's files (no game)
    py -I tools/goto/moments.py frame NAME@frame:F | NAME@event:KIND[:N]     the guest frame an address names (or why not)
    py -I tools/goto/moments.py track REF.csv RUN.csv --until F    compare two hash streams up to frame F (the track check)
    py -I tools/goto/moments.py wayback MANIFEST.json              a case zip's way_back -> a moment: address (or what is missing)
    py -I tools/goto/moments.py index NAME EVENTS.jsonl [--place ADDRESS] [--allow-short]   S101 lane 102: the bus index of a moment (moments_bus.py)
    py -I tools/goto/moments.py find WHEN [--place ADDRESS]       every moment whose bus index satisfies the WHEN (tools/goto/when.py), cheapest first
    py -I tools/goto/moments.py frame NAME@WHEN                   ... also: the frame a WHEN names in a moment's index

The library is out/moments/ (game-derived: local only, gitignored, a KEEP file): one folder per moment, holding
    NAME.rec    the recording (src/replay.h format, docs/formats/replay_rec.md)
    det.csv     optional: the recording's own hash stream (--wwe2k14_det_hash, src/frame_hash.h), the reference for the track check
    moment.json {name, rec, det, shows, build, frames, seed, events, rec_sha256, added}
RDR_MOMENTS_DIR overrides the library folder (the selftest uses it). Nothing here starts a game: tools/goto/resolve.py's executor does,
with --wwe2k14_replay_until=F (src/replay.h, S101 lane31), and calls track_check on the way.
"""
import argparse
import csv
import hashlib
import json
import os
import re
import shutil
import sys
import time
from dataclasses import dataclass, field

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
NAME_RE = re.compile(r'^[A-Za-z0-9_.\-]+$')
HASH_RE = re.compile(r'^(?:[0-9a-f]{16}|-|0|!)$')  # tools/determinism/det_compare.py's cell rule (v2: '0' null link, '!' unreadable)


def main_checkout():
    env = os.environ.get('RDR_MAIN_CHECKOUT')
    if env:
        return env
    if os.path.isdir(os.path.join(REPO, 'game_data')):
        return REPO
    up = os.path.abspath(os.path.join(REPO, '..', '..', '..'))
    return up if os.path.isdir(os.path.join(up, 'game_data')) else REPO


def library_dir():
    return os.environ.get('RDR_MOMENTS_DIR') or os.path.join(main_checkout(), 'out', 'moments')


class MomentError(ValueError):
    """The named moment, frame or event does not exist, or a file cannot be read: the message says which and why."""


if HERE not in sys.path:
    sys.path.insert(0, HERE)
import moments_bus as bus  # noqa: E402  S101 lane 102: the bus index (additive; sibling module)
import when as W  # noqa: E402  S101 lane 102


# ---------------------------------------------------------------- the recording (src/replay.h Load, mirrored)

@dataclass
class Rec:
    path: str
    version: int = 1
    name: str = ''
    seed: int = -1
    dlc: str = ''
    flags: dict = field(default_factory=dict)
    end_frame: int = 0            # the game's end_frame_: the max frame of every S / K / C / T / E line (replay.h Load)
    events: list = field(default_factory=list)   # [(frame, text)] in file order
    totals: dict = field(default_factory=dict)   # {frame: controller calls so far}
    lines: int = 0
    problems: list = field(default_factory=list)
    notes: list = field(default_factory=list)


def parse_rec(path):
    """Reads a .rec the way src/replay.h Load does (same end_frame). Raises MomentError when the file cannot be read; malformed lines
    go to .problems (the game skips what it cannot use, a check must say so)."""
    try:
        with open(path, encoding='utf-8', errors='strict', newline='') as f:
            text = f.read()
    except (OSError, UnicodeDecodeError) as x:
        raise MomentError('cannot read recording %s: %s' % (path, x))
    r = Rec(path)
    last_by_key = {}
    pieces = text.split('\n')
    if pieces[-1] != '':
        # A live copy of a recording being written can end mid-line (the recorder flushes at the T line every 60 frames, the CRT may spill
        # earlier). The game reads such a line as far as it parses; here the unfinished line is dropped and noted, never judged.
        r.notes.append('the last line has no newline (a copy of a recording being written): dropped')
        pieces = pieces[:-1]
    for ln_no, raw in enumerate(pieces, 1):
        line = raw[:-1] if raw.endswith('\r') else raw
        if line.startswith('# wwe2k14 replay v'):
            m = re.match(r'# wwe2k14 replay v(\d+)', line)
            r.version = int(m.group(1)) if m else 1
        if not line or line[0] == '#':
            continue
        tag, _, rest = line.partition(' ')
        bad = lambda why: r.problems.append('line %d: %s (%s)' % (ln_no, why, line[:60]))
        try:
            if tag == 'name':
                r.name = rest.strip()
            elif tag == 'seed':
                r.seed = int(rest.split()[0])
            elif tag == 'dlc':
                r.dlc = rest.strip()
            elif tag == 'flag':
                n, _, v = rest.partition(' ')
                r.flags[n] = v.strip()
            elif tag == 'T':
                f, n = rest.split()
                f, n = int(f), int(n)
                r.totals[f] = n
                r.end_frame = max(r.end_frame, f)
                if f % 60:
                    bad('T line off the 60-frame grid')
            elif tag == 'E':
                f, _, t = rest.partition(' ')
                f = int(f)
                r.events.append((f, t.strip()))
                r.end_frame = max(r.end_frame, f)
            elif tag in ('S', 'K', 'C'):
                parts = rest.split()
                f, k = int(parts[0]), int(parts[1])
                user, flags, ret = int(parts[2], 16), int(parts[3], 16), int(parts[4], 16)
                hexs = parts[-1]
                if tag == 'K':
                    int(parts[5], 16)
                if hexs != '-' and (len(hexs) % 2 or re.search(r'[^0-9A-Fa-f]', hexs)):
                    bad('bad hex')
                key = (tag, user, flags)
                if key in last_by_key and (f, k) < last_by_key[key]:
                    bad('lines out of order')
                last_by_key[key] = (f, k)
                r.end_frame = max(r.end_frame, f)
                r.lines += 1
            else:
                bad('unknown line tag "%s"' % tag)
        except (ValueError, IndexError):
            bad('malformed %s line' % tag)
    if r.end_frame == 0:
        r.problems.append('no frames: the file holds no S / K / C / T / E line (an empty or wrong file)')
    if r.seed < 0:
        r.problems.append('no seed line: the game would pick one from the clock and the run would not repeat')
    return r


# ---------------------------------------------------------------- addresses -> frames

def resolve_frame(rec, at_frame=-1, event_kind='', event_n=0):
    """-> the guest frame the address names, or raises MomentError. A frame past the recording's end, or an event the recording
    lacks, is an error, never a plan (the game would only hand over at the recording's end, at the wrong place)."""
    if at_frame >= 0:
        if at_frame > rec.end_frame:
            raise MomentError('frame %d is past the recording\'s last frame %d (the game hands over at the end of the recording, '
                              'not at the frame asked for)' % (at_frame, rec.end_frame))
        if at_frame == 0:
            raise MomentError('frame 0 is the boot: --wwe2k14_replay_until=0 means off; name frame 1 or later')
        return at_frame
    hits = [f for f, t in rec.events if t.split(' ')[0] == event_kind or t.startswith(event_kind + ' ') or t == event_kind]
    if not hits:
        kinds = sorted({t.split(' ')[0] for _, t in rec.events})
        raise MomentError('the recording has no event "%s" (its events: %s)' % (event_kind, ', '.join(kinds) or 'none'))
    if event_n >= len(hits):
        raise MomentError('the recording has %d "%s" event(s); event %d does not exist' % (len(hits), event_kind, event_n))
    if hits[event_n] < 1:
        raise MomentError('the event is at frame %d: frame 0 means "replay_until off"; no hand-over there' % hits[event_n])
    return hits[event_n]


# ---------------------------------------------------------------- the library

def moment_dir(name, lib=None):
    return os.path.join(lib or library_dir(), name)


def load_moment(name, lib=None):
    """-> (meta dict, Rec). Raises MomentError when it is not registered or its files are missing / changed."""
    if not NAME_RE.match(name or ''):
        raise MomentError('"%s" is not a moment name (letters, digits, _ . -)' % name)
    d = moment_dir(name, lib)
    mj = os.path.join(d, 'moment.json')
    if not os.path.isfile(mj):
        raise MomentError('no moment "%s" in %s (py -I tools/goto/moments.py list)' % (name, lib or library_dir()))
    try:
        with open(mj, encoding='utf-8') as f:
            meta = json.load(f)
    except (OSError, ValueError) as x:
        raise MomentError('moment.json of "%s" is unreadable: %s' % (name, x))
    rec_path = os.path.join(d, meta.get('rec', ''))
    if not os.path.isfile(rec_path):
        raise MomentError('moment "%s": the recording %s is missing' % (name, rec_path))
    rec = parse_rec(rec_path)
    sha = hashlib.sha256(open(rec_path, 'rb').read()).hexdigest()
    if not meta.get('rec_sha256'):
        raise MomentError('moment "%s": moment.json has no rec_sha256 (cannot tell whether the recording is the one that was added): add it again' % name)
    if meta['rec_sha256'] != sha:
        raise MomentError('moment "%s": the recording changed since it was added (sha256 differs): add it again' % name)
    if meta.get('det'):
        dp = os.path.join(d, meta['det'])
        if not os.path.isfile(dp):
            raise MomentError('moment "%s": the hash stream %s is missing' % (name, dp))
        if not meta.get('det_sha256'):
            raise MomentError('moment "%s": moment.json has no det_sha256: add it again' % name)
        if hashlib.sha256(open(dp, 'rb').read()).hexdigest() != meta['det_sha256']:
            raise MomentError('moment "%s": the hash stream changed since it was added (sha256 differs): add it again' % name)
    return meta, rec


def list_moments(lib=None):
    lib = lib or library_dir()
    out = []
    if os.path.isdir(lib):
        for n in sorted(os.listdir(lib)):
            if os.path.isfile(os.path.join(lib, n, 'moment.json')):
                out.append(n)
    return out


def add_moment(name, rec_file, det_csv=None, shows='', build='', lib=None, data='', events=None, place='', allow_short=False):
    """Registers a recording: copies it as NAME.rec (and the hash stream as det.csv) and writes moment.json. Refuses a recording that
    does not parse, a name that exists, and a hash stream that does not parse."""
    if not NAME_RE.match(name or ''):
        raise MomentError('"%s" is not a moment name (letters, digits, _ . -)' % name)
    lib = lib or library_dir()
    d = moment_dir(name, lib)
    if os.path.exists(d):
        raise MomentError('moment "%s" exists already (%s): pick another name' % (name, d))
    rec = parse_rec(rec_file)
    if rec.problems:
        raise MomentError('recording %s does not validate: %s' % (rec_file, '; '.join(rec.problems[:3])))
    if det_csv:
        parse_det(det_csv)  # raises on an unreadable / headerless stream
    os.makedirs(d)
    dst = os.path.join(d, name + '.rec')
    shutil.copyfile(rec_file, dst)
    if det_csv:
        shutil.copyfile(det_csv, os.path.join(d, 'det.csv'))
    det_sha = hashlib.sha256(open(os.path.join(d, 'det.csv'), 'rb').read()).hexdigest() if det_csv else None
    meta = {'name': name, 'rec': name + '.rec', 'det': 'det.csv' if det_csv else None, 'det_sha256': det_sha, 'shows': shows, 'build': build,
            'data': data,  # the frozen save copy under out\bench it was recorded on (live.start(data=...)); '' = the default one
            'frames': rec.end_frame, 'seed': rec.seed, 'events': len(rec.events),
            'rec_sha256': hashlib.sha256(open(dst, 'rb').read()).hexdigest(), 'added': time.strftime('%Y-%m-%d %H:%M')}
    if events:   # S101 lane 102: the run's bus stream beside the recording
        try:
            bus.write_index(d, meta, events, rec.end_frame, place, allow_short)
        except W.WhenError as x:
            shutil.rmtree(d, ignore_errors=True)
            raise MomentError(str(x))
    elif place:
        meta['place'] = place
    with open(os.path.join(d, 'moment.json'), 'w', encoding='utf-8') as f:
        json.dump(meta, f, indent=1)
    keep = os.path.join(lib, 'KEEP')
    if not os.path.exists(keep):
        with open(keep, 'w') as f:
            f.write('moments: recordings and hash streams of scenes Jarrod played (tools/goto/moments.py); game-derived, never committed\n')
    return meta


def check_moment(name, lib=None):
    """No game. -> list of problems (empty = fine): recording parses and has frames and a seed; the hash stream (if any) parses and covers
    the recording to its last 60-frame block; moment.json's frame count matches the file."""
    problems = []
    try:
        meta, rec = load_moment(name, lib)
    except MomentError as x:
        return [str(x)]
    problems += rec.problems
    if meta.get('frames') != rec.end_frame:
        problems.append('moment.json says %s frames, the recording has %d' % (meta.get('frames'), rec.end_frame))
    if meta.get('bus_events'):   # S101 lane 102
        try:
            bus.load_bus(moment_dir(name, lib), dict(meta, name=name))
        except W.WhenError as x:
            problems.append(str(x))
    if meta.get('det'):
        try:
            cols, rows, _ = parse_det(os.path.join(moment_dir(name, lib), meta['det']))
            last = max(rows) if rows else 0
            if last < rec.end_frame - 60:
                problems.append('the hash stream ends at frame %d, the recording at %d: the track check cannot cover the end' % (last, rec.end_frame))
        except MomentError as x:
            problems.append(str(x))
    return problems


# ---------------------------------------------------------------- S101 lane 102: the bus index (WHEN -> frame)

def load_indexed(name, lib=None):
    """-> (meta, rec, events, dir) of a moment with an event index; MomentError when it has none or the index changed."""
    meta, rec = load_moment(name, lib)
    d = moment_dir(name, lib)
    try:
        events = bus.load_bus(d, dict(meta, name=name))
    except W.WhenError as x:
        raise MomentError(str(x))
    return meta, rec, events, d


def when_frame(name, when_text, lib=None):
    """-> (frame, Hit, meta, rec): the guest frame a WHEN names in the moment's bus index. A WHEN the index lacks, a frame outside the
    recording (1..end_frame: the game hands over inside the recording only) or a missing index is a MomentError saying what the index holds."""
    meta, rec, events, _ = load_indexed(name, lib)
    try:
        h = W.evaluate(events, when_text, bus.entrants_of(meta))
    except W.WhenError as x:
        raise MomentError('moment "%s": %s' % (name, x))
    if h.frame > rec.end_frame:
        raise MomentError('moment "%s": %s names frame %d, past the recording\'s last frame %d' % (name, when_text, h.frame, rec.end_frame))
    return h.frame, h, meta, rec


def find_moments(when_text, place=None, lib=None):
    """-> ([(frame, name, Hit)] cheapest first, [(name, why skipped)])."""
    return bus.find(list_moments(lib), lambda n: load_indexed(n, lib), when_text, place)


# ---------------------------------------------------------------- the track check (hash streams)

def parse_det(path):
    """A det_*.csv (src/frame_hash.h): '#' lines, then a header row with a 'frame' column, then one row per frame of hex cells
    ('-' null / unreadable, '0' null link, '!' unreadable memory in v2). -> (columns, {frame: {col: cell}}, comment lines).
    Raises MomentError on an unreadable file, a missing header or a row that does not fit it (a checker that reads nothing fails)."""
    try:
        with open(path, encoding='utf-8', newline='') as f:
            raw = f.read().replace('\r\n', '\n').split('\n')
        # frame_hash.h flushes every 60 frames and the CRT spills its buffer mid-line: a last line with no newline may be cut off
        # (det_compare.py drops it too). raw[-1] is '' when the file ends with a newline, else it is the unfinished line.
        raw = raw[:-1]
    except (OSError, UnicodeDecodeError) as x:
        raise MomentError('cannot read hash stream %s: %s' % (path, x))
    comments = [ln for ln in raw if ln.startswith('#')]
    body = [ln for ln in raw if ln and not ln.startswith('#')]
    if not body:
        raise MomentError('hash stream %s has no header row' % path)
    cols = body[0].split(',')
    if 'frame' not in cols:
        raise MomentError('hash stream %s: the header row has no "frame" column (got %s)' % (path, body[0][:60]))
    fi = cols.index('frame')
    rows = {}
    for n, ln in enumerate(body[1:], 2):
        cells = ln.split(',')
        if len(cells) != len(cols):
            raise MomentError('hash stream %s: row %d has %d cells, the header %d' % (path, n, len(cells), len(cols)))
        try:
            frame_no = int(cells[fi])
        except ValueError:
            raise MomentError('hash stream %s: row %d has no frame number' % (path, n))
        for c, v in zip(cols, cells):
            if c not in ('frame', 'vb') and not HASH_RE.match(v):  # det_compare.py's rule: a cut or corrupt cell is not a hash
                raise MomentError('hash stream %s: row %d column %s is not a hash cell (%r)' % (path, n, c, v[:20]))
        rows[frame_no] = dict(zip(cols, cells))
    return cols, rows, comments


@dataclass
class Track:
    ok: bool
    compared: int = 0                 # frames compared
    first_slip: int = -1              # first frame whose cells differ
    columns: list = field(default_factory=list)   # the columns that differ at first_slip
    unreadable: int = 0               # cells skipped because either side could not read the region ('-' / '!')
    reason: str = ''

    def line(self):
        if self.ok:
            return 'track OK: %d frames compared, 0 slips (%d unreadable cells skipped)' % (self.compared, self.unreadable)
        return 'track STOPPED at frame %d: %s' % (self.first_slip, self.reason)


def track_check(ref_csv, run_csv, until, from_frame=1):
    """Compares the reference stream with the run's up to guest frame `until`. The first frame where any region's hash differs stops
    it, naming the frame and the regions. A run stream that ends before `until`, a reference that does not reach it, or a frame the
    run lacks in the middle is a stop too (never a silent pass). 'frame' / 'vb' columns: vb (the det vblank count) is compared, it is
    the scheduler's key."""
    rc, rr, _ = parse_det(ref_csv)
    uc, ur, _ = parse_det(run_csv)
    if not rr or max(rr) < until:
        return Track(False, first_slip=until, reason='the reference stream ends at frame %d, before the frame asked for (%d): nothing to check against' % (max(rr) if rr else 0, until))
    shared = [c for c in rc if c in uc and c != 'frame']
    if not shared:
        return Track(False, first_slip=1, reason='the two streams share no region column (reference %s / run %s): different --wwe2k14_det_hash specs' % (rc[:4], uc[:4]))
    t = Track(True)
    for f in range(from_frame, until + 1):
        if f not in rr:
            continue  # the reference has no row there (a gap in the old run): nothing to compare
        if f not in ur:
            if f > max(ur, default=0):
                return Track(False, t.compared, f, reason='the run\'s stream ends at frame %d, before the frame asked for (%d)' % (max(ur, default=0), until))
            return Track(False, t.compared, f, reason='the run\'s stream has no row for frame %d' % f)
        bad = []
        for c in shared:
            a, b = rr[f][c], ur[f][c]
            if a in ('-', '!') or b in ('-', '!'):
                t.unreadable += 1
                continue
            if a != b:
                bad.append(c)
        t.compared += 1
        if bad:
            return Track(False, t.compared, f, bad, t.unreadable,
                         'the run\'s regions differ from the reference: %s (the recording is not frame-exact on this build, or the setup differs)' % ', '.join(bad[:8]))
    return t


# ---------------------------------------------------------------- the way back from a case zip

def wayback(manifest, case_dir=None, lib=None, data='', current_build=None):
    """A case manifest's way_back (docs/renderer/CASE_ZIP.md) -> (address, notes), or raises MomentError saying exactly what is missing.

    Frame-exact needs the recording from boot: the controller calls of the WHOLE session (`session.rec`, the replay recorder's file,
    cut at moment_frame) and the game started in replay mode (the recording is only valid when det_frames / det_serial ran from boot).
    The window's `inputs.rec` (first_frame..moment_frame) is not enough: the prefix since boot is missing, and without it the game state at
    first_frame is not reproduced. A zip with only the builder gets that builder's address: reachable, not frame-exact."""
    wb = manifest.get('way_back') or {}
    mf = wb.get('moment_frame')
    builder = wb.get('builder')
    sess = os.path.join(case_dir, 'session.rec') if case_dir else None
    notes = []
    if wb.get('replay_mode') and sess and os.path.isfile(sess):
        rec = parse_rec(sess)
        if rec.problems:
            raise MomentError('the case\'s session.rec does not validate: %s' % '; '.join(rec.problems[:2]))
        if mf is None or mf <= 0:
            raise MomentError('the case has no moment_frame')
        if rec.end_frame < mf:
            raise MomentError('the case\'s session.rec ends at frame %d, before the moment (%d): the recorder writes pad lines on change and '
                              'flushes at the T line every 60 frames, so the case writer must call Replay::RecordEvent(moment_frame, "case") '
                              '(an "E F case" line, flushed under the recorder\'s mutex) before it copies the file' % (rec.end_frame, mf))
        if wb.get('seed') is not None and rec.seed >= 0 and int(wb['seed']) != rec.seed:
            raise MomentError('the case\'s seed %s is not the session.rec\'s %d' % (wb['seed'], rec.seed))
        name = 'case_' + re.sub(r'[^A-Za-z0-9_.\-]', '_', str(manifest.get('id', 'case')))
        mbuild = json.dumps(manifest.get('build') or '')
        add_moment(name, sess, shows=(manifest.get('where') or {}).get('words') or '', build=mbuild, lib=lib, data=data)
        notes.append('frame-exact IF the same save data, the same build and the same mods as the case\'s session: the recording holds pad '
                     'calls only (no save, no build, no mods)')
        if manifest.get('build') and current_build is not None and json.dumps(manifest['build']) != json.dumps(current_build):
            notes.append('WARNING: the case was made on another build (%s) than the one that will play it (%s): not frame-exact until they match'
                         % (json.dumps(manifest['build'])[:80], json.dumps(current_build)[:80]))
        elif not manifest.get('build'):
            notes.append('the manifest names no build: same-build is unchecked')
        notes.append('no hash stream in a case zip: the track check is skipped')
        return 'moment:%s@frame:%d' % (name, mf), notes
    why = []
    if not wb.get('replay_mode'):
        why.append('the game was not in replay mode from boot (way_back.replay_mode is false): its frames are not reproducible')
    if not (sess and os.path.isfile(sess)):
        why.append('the zip carries no session.rec (inputs.rec holds only the window %s..%s; the prefix since boot is missing)' % (wb.get('first_frame'), mf))
    if builder:
        notes.append('not frame-exact: ' + '; '.join(why))
        return builder if ':' in builder else 'match:' + builder, notes
    raise MomentError('no way back: ' + '; '.join(why) + '; and no builder address either')


# ---------------------------------------------------------------- CLI

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('cmd', choices=['list', 'add', 'check', 'frame', 'track', 'wayback', 'index', 'find'])
    ap.add_argument('args', nargs='*')
    ap.add_argument('--det', default='')
    ap.add_argument('--shows', default='')
    ap.add_argument('--build', default='')
    ap.add_argument('--data', default='', help='add: the frozen save copy under out\\bench the recording was made on (passed to live.start(data=...))')
    ap.add_argument('--until', type=int, default=0)
    ap.add_argument('--events', default='', help="add: the run's events_NNN.jsonl (the bus index, S101 lane 102)")
    ap.add_argument('--place', default='', help='add / index / find: the builder address the recording was made from, e.g. "match:Daniel Bryan vs Brock Lesnar; players=0"')
    ap.add_argument('--allow-short', action='store_true', help='index: accept a bus stream that ends well before the recording')
    a = ap.parse_args(argv)
    try:
        if a.cmd == 'list':
            for n in list_moments():
                m, _ = load_moment(n)
                print('%-24s %6d frames  %2d events  det=%s  %s' % (n, m['frames'], m['events'], 'yes' if m.get('det') else 'no', m.get('shows', '')))
            return 0
        if a.cmd == 'add':
            if len(a.args) != 2:
                ap.error('add NAME RECFILE')
            print(json.dumps(add_moment(a.args[0], a.args[1], a.det or None, a.shows, a.build, data=a.data, events=a.events or None, place=a.place,
                                        allow_short=a.allow_short), indent=1))
            return 0
        if a.cmd == 'index':   # S101 lane 102
            if len(a.args) != 2:
                ap.error('index NAME EVENTS.jsonl')
            meta, rec = load_moment(a.args[0])
            try:
                bus.write_index(moment_dir(a.args[0]), meta, a.args[1], rec.end_frame, a.place or meta.get('place', ''), a.allow_short)
            except W.WhenError as x:
                raise MomentError(str(x))
            with open(os.path.join(moment_dir(a.args[0]), 'moment.json'), 'w', encoding='utf-8') as f:
                json.dump(meta, f, indent=1)
            print('indexed %s: %s' % (a.args[0], json.dumps(meta['index']['counts'])))
            return 0
        if a.cmd == 'find':
            if len(a.args) != 1:
                ap.error('find WHEN')
            hits, skipped = find_moments(a.args[0], a.place or None)
            for f, n, h in hits:
                print('%8d  %-28s %s' % (f, n, json.dumps(h.line)))
            for n, why in skipped:
                print('  skipped %s: %s' % (n, why))
            if not hits:
                print('no moment satisfies %s' % a.args[0])
            return 0 if hits else 1
        if a.cmd == 'check':
            names = a.args or list_moments()
            bad = 0
            for n in names:
                p = check_moment(n)
                bad += bool(p)
                print('%s  %s' % ('FAIL' if p else 'ok  ', n) + ('  ' + '; '.join(p) if p else ''))
            return 1 if bad else 0
        if a.cmd == 'frame':
            from address import parse
            ad = parse('moment:' + a.args[0])
            if ad.when:   # S101 lane 102
                f, h, _, _ = when_frame(ad.body, ''.join(ad.when))
                print(f)
                return 0
            _, rec = load_moment(ad.body)
            print(resolve_frame(rec, ad.frame, ad.event_kind, ad.event_n))
            return 0
        if a.cmd == 'track':
            t = track_check(a.args[0], a.args[1], a.until)
            print(t.line())
            return 0 if t.ok else 1
        if a.cmd == 'wayback':
            with open(a.args[0], encoding='utf-8') as f:
                man = json.load(f)
            adr, notes = wayback(man, os.path.dirname(os.path.abspath(a.args[0])))
            print(adr)
            for n in notes:
                print('  note: ' + n)
            return 0
    except MomentError as x:
        print('REFUSED: %s' % x)
        return 1
    return 0


if __name__ == '__main__':
    sys.path.insert(0, HERE)
    sys.exit(main())
