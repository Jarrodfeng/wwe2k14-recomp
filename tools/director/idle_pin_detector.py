"""S101 lane 161: the past-bug re-find for the situation director - "the CPU pinned a wrestler nobody defended" (Check J3, run 7092).

    py -I tools/director/idle_pin_detector.py EVENTS.jsonl [--log RUN.log] [--window FRAMES] [--json]

Reads a run's event-bus file (out\\build\\...\\logs\\events_NNNN.jsonl; the same lines are in the run log as `[wwe2k14][event] {...}`; pass the .log as
EVENTS and it reads those) and flags every pinfall in which the pinned wrestler gave the game no sign of a player:
  * the bus saw a `pin` (pinning_slot P) followed by a `finish` pinfall (winner W, loser L);
  * between the pin and the finish there was no `kickout` (a defender who mashes escapes at least sometimes), and
  * the loser L made no `move` in the WINDOW guest frames before the pin (default 1800 = 30 s) or during it (the bus `move` lines, src/event_bus_rumble.h).
The verdict is `no_defender`: a pinfall nobody defended, exactly Check J3, run 7092 (the CPU Andre pinned an idle Hogan after the 600-frame hold), and the situation the
director's `passive` verb makes on purpose. With --log, a `director: passive L on` / `passive all on` line before the pin marks it "passive (director)": a
controlled idle, reported as expected, not as a flag.

Exit: 0 = no unexplained no-defender pin (a clean run, or no pin at all), 3 = at least one, 2 = the input cannot be read. A pin with no finish after it
(a kick-out the bus did not see, the run ended) is reported "unfinished" and never flagged.
Proven on: run 7092 (must say no_defender) and run 5910 (must not): tools/director/director_selftest.py.
NOT a pass / fail gate for a route yet: over the 55 pins on disk 24 are no_defender, 1 clean (5910), the rest were kicked out, because the CPU rarely defends and the bus
`move` option (the lines that show a player's activity) is off in every historical log. It becomes a gate only for runs recorded with that option on.
"""
import json
import re
import sys

WINDOW_DEFAULT = 1800
EVENT_RE = re.compile(r'\[wwe2k14\]\[event\]\s*(\{.*\})\s*$')
PASSIVE_ONE_RE = re.compile(r'director: passive (\d) (on|off) \(')   # the per-slot line every passive on / off (and passive all / passive_cpu) logs


def read_events(path):
    """The bus events (dicts) of a jsonl file or of a run log; the passive_by_director lines of a log as {'kind': 'director_passive', 'slot', 'line'} (frame -1: log order)."""
    events, passive = [], []
    try:
        with open(path, encoding='utf-8', errors='replace') as f:
            lines = f.read().splitlines()
    except OSError as x:
        raise SystemExit('cannot read %s: %s' % (path, x))
    last_frame = 0
    for ln, line in enumerate(lines):
        m = EVENT_RE.search(line)
        raw = m.group(1) if m else (line if line.startswith('{') else None)
        if raw:
            try:
                e = json.loads(raw)
            except ValueError:
                continue
            if isinstance(e, dict) and 'kind' in e:
                last_frame = e.get('f', last_frame)
                events.append(e)
            continue
        m = PASSIVE_ONE_RE.search(line)
        if m:
            passive.append({'slot': int(m.group(1)), 'on': m.group(2) == 'on', 'after_frame': last_frame if events else None, 'line': ln + 1})   # None: before any bus line
    return events, passive


def analyse(events, passive=(), window=WINDOW_DEFAULT):
    """Returns a list of dicts, one per pin: {pin_frame, pinner, loser, winner, finish_frame, verdict, why}. verdict: no_defender | clean | passive | unfinished."""
    out = []
    pins = [e for e in events if e.get('kind') == 'match' and e.get('what') == 'pin']
    for i, p in enumerate(pins):
        fp = p['f']
        nxt = pins[i + 1]['f'] if i + 1 < len(pins) else 10 ** 12
        fin = next((e for e in events if e.get('kind') == 'match' and e.get('what') == 'finish' and fp < e['f'] <= nxt and e.get('finish_type') == 1), None)
        rec = {'pin_frame': fp, 'pinner': p.get('pinning_slot'), 'loser': None, 'winner': None, 'finish_frame': None}
        if not fin:
            rec.update(verdict='unfinished', why='no pinfall finish followed this pin (a kick-out, or the run ended)')
            out.append(rec)
            continue
        loser, ff = fin.get('loser_slot'), fin['f']
        rec.update(loser=loser, winner=fin.get('winner_slot'), finish_frame=ff)
        kicks = [e for e in events if e.get('kind') == 'match' and e.get('what') == 'kickout' and fp <= e['f'] <= ff]
        moves = [e for e in events if e.get('kind') == 'move' and e.get('slot') == loser and fp - window <= e['f'] <= ff]
        counts = [e.get('count') for e in events if e.get('kind') == 'match' and e.get('what') == 'count' and fp <= e['f'] <= ff]
        rec['counts'] = counts
        before = [q for q in passive if q['slot'] == loser and (q['after_frame'] is None or q['after_frame'] <= fp)]
        by_director = bool(before) and before[-1].get('on', True)   # the latest passive line for the loser decides: an `off` after an `on` ends the explanation
        if kicks:
            rec.update(verdict='clean', why='the pinned wrestler kicked out at count %s before the finish (a defender with input)' % kicks[0].get('at_count'))
        elif moves:
            rec.update(verdict='clean', why='slot %s made %d move(s) in the %d frames before the pin' % (loser, len(moves), window))
        elif by_director:
            rec.update(verdict='passive', why='slot %s was made passive by the director before the pin: a controlled idle, expected' % loser)
        else:
            rec.update(verdict='no_defender', why='the CPU in slot %s pinned slot %s: no kickout, no move from it in the %d frames before the pin, counts %s, and it was not made passive '
                                           'by the director (Check J3, run 7092: nobody defended)' % (rec['pinner'], loser, window, counts))
        out.append(rec)
    return out


def main(argv):
    args = [a for a in argv if not a.startswith('--')]
    window = WINDOW_DEFAULT
    log = None
    if '--window' in argv:
        window = int(argv[argv.index('--window') + 1])
        args.remove(str(window))
    if '--log' in argv:
        log = argv[argv.index('--log') + 1]
        args.remove(log)
    if len(args) != 1:
        print(__doc__)
        return 2
    events, passive = read_events(args[0])
    if log:
        _, passive = read_events(log)
    if not events:
        print('no bus events in %s' % args[0])
        return 2
    result = analyse(events, passive, window)
    if '--json' in argv:
        print(json.dumps(result, indent=1))
    else:
        if not result:
            print('no pin in this run')
        for r in result:
            print('%-10s pin at frame %s by slot %s%s: %s' % (r['verdict'].upper(), r['pin_frame'], r['pinner'], '' if r['loser'] is None else ' of slot %s (finish at %s)' % (r['loser'], r['finish_frame']), r['why']))
    return 3 if any(r['verdict'] == 'no_defender' for r in result) else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
