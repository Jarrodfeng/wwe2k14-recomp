"""S101 lane 102 (go-anywhere L-1): the bus index of a library moment. A recording plus the event-bus stream of the run that made it
(logs/events_NNN.jsonl, src/event_bus.h) = every pin, count, kickout, elimination, entrance step, finish and load of that run addressable
as a guest frame, offline: `moment:NAME@WHEN` (tools/goto/when.py) resolves through here.

    py -I tools/goto/moments.py index NAME EVENTS.jsonl [--place ADDRESS] [--allow-short]
    py -I tools/goto/moments.py find WHEN [--place ADDRESS]
    py -I tools/goto/moments.py frame NAME@WHEN                       (also: moments.py frame NAME@frame:F)

The index lives in the moment's folder (out/moments/NAME/, gitignored like the rest of the library):
    events.jsonl   the bus stream cut at the recording's last frame (our log format; local only)
    moment.json    + place, bus_events, bus_sha256, index {counts, first/last frame per kind, wrestlers, entrants}, index_sha256
The event track (event_track below, called by tools/goto/resolve.py's moment_wait): every bus line of the replay before the hand-over frame F
re-occurs at its indexed frame, none missing, none extra. A divergence in the bus is the earliest sign a recording stopped being frame-exact
on this build (the hash track needs a det.csv; this one needs only the events file).
This file is imported by moments.py (additive); nothing here starts a game.
"""
import hashlib
import json
import os
import re
from dataclasses import dataclass

import when as W

TRACK_SKIP_KINDS = ('state',)   # on request only (--wwe2k14_match_state_dump): a harvest may hold some, a replay does not ask for them
EVENTS_FILE = 'events.jsonl'


def _sha(path):
    with open(path, 'rb') as f:
        return hashlib.sha256(f.read()).hexdigest()


def canon_place(text):
    """The place of a recording / an address as comparable text: lowercase, spaces collapsed, the `;` fields after the first sorted, a
    leading `match:` dropped. Two spellings of the same builder spec in another field order compare equal. (Not the atlas-level canonical
    form: resolve.raw_spec / match.build_spec do that and need the game's atlas; a recording stores the spec text it was made from.)"""
    t = re.sub(r'\s+', ' ', (text or '').strip().lower())
    if t.startswith('match:'):
        t = t[6:].strip()
    parts = [p.strip() for p in t.split(';') if p.strip()]
    return ';'.join(parts[:1] + sorted(parts[1:]))


def summarize(events, rec_end):
    kinds, first, last = {}, {}, {}
    for e in events:
        k = e['kind'] + (':' + e['what'] if e['kind'] == 'match' and 'what' in e else '')
        kinds[k] = kinds.get(k, 0) + 1
        first.setdefault(k, e['f'])
        last[k] = e['f']
    wrestlers = []
    bell = next((e['f'] for e in events if e['kind'] == 'match' and e.get('what') == 'clock_started'), None)
    if bell is not None:
        st = next((e for e in events if e['kind'] == 'state' and e['f'] >= bell and isinstance(e.get('wrestlers'), list)), None)
        if st:
            wrestlers = [w.get('id') for w in st['wrestlers'] if isinstance(w, dict)]
    return {'counts': kinds, 'first': first, 'last': last, 'last_frame': max((e['f'] for e in events), default=0), 'rec_frames': rec_end,
            'wrestlers': wrestlers, 'entrants': None}


def index_json_sha(idx):
    return hashlib.sha256(json.dumps(idx, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def build_index(rec_end, events_path, allow_short=False):
    """-> (events cut at rec_end, summary). Refuses an empty stream, a stream without a seq event (not from a bus run), a stream that goes
    back in time, and one that stops well before the recording's end (a bus stream of a shorter run than the recording) unless allow_short."""
    events = W.load_events(events_path)
    if not events:
        raise W.WhenError('the event stream %s is empty (was the run started with --wwe2k14_events=true?)' % events_path)
    if not any(e['kind'] == 'seq' for e in events):
        raise W.WhenError('the event stream %s has no "seq" line: not a bus stream of a whole run' % events_path)
    prev = -1
    for e in events:
        if e['f'] < prev:
            raise W.WhenError('the event stream %s goes back in time (frame %d after %d): not one run\'s stream' % (events_path, e['f'], prev))
        prev = e['f']
    cut = [e for e in events if e['f'] <= rec_end]
    last = max(e['f'] for e in cut) if cut else 0
    if not allow_short and last < rec_end - 600:
        raise W.WhenError('the event stream ends at frame %d, the recording at %d (%d frames earlier): the stream is of a shorter run or was cut; '
                          'every WHEN after frame %d would be missing (--allow-short registers it anyway)' % (last, rec_end, rec_end - last, last))
    return cut, summarize(cut, rec_end)


def write_index(d, meta, events_path, rec_end, place='', allow_short=False):
    """Writes events.jsonl into the moment's folder and the index keys into `meta` (the caller saves moment.json). -> meta."""
    cut, idx = build_index(rec_end, events_path, allow_short)
    dst = os.path.join(d, EVENTS_FILE)
    with open(dst, 'w', encoding='utf-8', newline='') as f:
        for e in cut:
            f.write(json.dumps(e, separators=(', ', ': ')) + '\n')
    meta['bus_events'] = EVENTS_FILE
    meta['bus_sha256'] = _sha(dst)
    meta['index'] = idx
    meta['index_sha256'] = index_json_sha(idx)
    if place:
        meta['place'] = place
    return meta


def load_bus(d, meta):
    """-> [events] of a moment folder, verified against moment.json; raises W.WhenError (no index / changed since indexed)."""
    name = meta.get('name', os.path.basename(d))
    if not meta.get('bus_events'):
        raise W.WhenError('moment "%s" has no event index (it was added without the run\'s events): py -I tools/goto/moments.py index %s EVENTS.jsonl' % (name, name))
    p = os.path.join(d, meta['bus_events'])
    if not os.path.isfile(p):
        raise W.WhenError('moment "%s": the event index %s is missing' % (name, p))
    if _sha(p) != meta.get('bus_sha256'):
        raise W.WhenError('moment "%s": events.jsonl changed since it was indexed (sha256 differs): index it again' % name)
    if index_json_sha(meta.get('index', {})) != meta.get('index_sha256'):
        raise W.WhenError('moment "%s": moment.json\'s index changed since it was written (sha256 differs): index it again' % name)
    return W.load_events(p)


def entrants_of(meta, place=''):
    idx = meta.get('index') or {}
    if idx.get('entrants'):
        return int(idx['entrants'])
    m = re.search(r'\brumble\s*=\s*(\d+(?:,\d+)*)', place or meta.get('place', ''), re.I)   # unconfirmed (R2): a Rumble's slot count
    if not m:
        return None
    ids = m.group(1).split(',')
    return len(ids) if len(ids) > 1 or int(ids[0]) >= 100 else int(ids[0])   # builder spec: rumble=ID,ID,... ; a user's spec: rumble=10 (random)


# ---------------------------------------------------------------- the event track

@dataclass
class EventTrack:
    ok: bool
    compared: int = 0          # frames with indexed lines that were compared
    first_diff: int = -1
    reason: str = ''

    def line(self):
        if self.ok:
            return 'event track OK: %d frames of bus lines compared, 0 differences' % self.compared
        return 'event track STOPPED at frame %d: %s' % (self.first_diff, self.reason)


def _key(e):
    return json.dumps({k: v for k, v in e.items() if k != 't'}, sort_keys=True, separators=(',', ':'))   # t = wall seconds: differs by run


def event_track(ref, run, until, complete=False):
    """Compares the indexed bus lines with a replay's. Every line with f < until must re-occur at its frame, none missing, none extra
    (same-frame lines compared as a set: their order within one guest frame is the bus tick's, not a recording property).
    While the run is still going (complete=False) only frames before the run's last seen frame are judged (more lines of that frame may
    follow); complete=True (the hand-over happened) judges every frame below `until`: a missing line is a stop, never a pass."""
    refs = {}
    for e in ref:
        if e['kind'] not in TRACK_SKIP_KINDS and e['f'] < until:
            refs.setdefault(e['f'], []).append(_key(e))
    runs = {}
    for e in run:
        if e['kind'] not in TRACK_SKIP_KINDS and e['f'] < until:
            runs.setdefault(e['f'], []).append(_key(e))
    seen = max((e['f'] for e in run), default=-1)
    limit = until if complete else min(until, seen)
    if complete and not any(e['kind'] not in TRACK_SKIP_KINDS for e in run) and refs:
        return EventTrack(False, 0, min(refs), 'the replay wrote no bus lines at all (started without --wwe2k14_events=true?)')
    t = EventTrack(True)
    for f in sorted(set(refs) | set(runs)):
        if f >= limit:
            break
        a, b = sorted(refs.get(f, [])), sorted(runs.get(f, []))
        if f in refs:
            t.compared += 1
        if a != b:
            miss = [x for x in a if x not in b]
            extra = [x for x in b if x not in a]
            why = ('missing from the replay: %s' % miss[0][:160]) if miss else ('not in the index: %s' % extra[0][:160])
            return EventTrack(False, t.compared, f, 'the replay\'s bus lines at frame %d differ from the index (%s)' % (f, why))
    return t


# ---------------------------------------------------------------- find

def find(lib_names, loader, when_text, place=None):
    """-> ([(frame, name, Hit)] smallest frame first, [(name, why skipped)]) for every library moment whose index satisfies the WHEN (and whose
    place equals `place`, if given). `loader(name)` -> (meta, rec, events, dir); moments without an index are skipped and listed."""
    out, skipped = [], []
    want = canon_place(place) if place else None
    for n in lib_names:
        try:
            meta, rec, events, _ = loader(n)
        except ValueError as x:   # MomentError and WhenError are ValueErrors
            skipped.append((n, str(x)))
            continue
        if want is not None and canon_place(meta.get('place', '')) != want:
            continue
        try:
            h = W.evaluate(events, when_text, entrants_of(meta))
        except W.WhenError:
            continue
        if 1 <= h.frame <= rec.end_frame:
            out.append((h.frame, n, h))
    out.sort(key=lambda x: (x[0], x[1]))
    return out, skipped
