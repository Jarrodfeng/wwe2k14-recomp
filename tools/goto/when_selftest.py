"""S101 lane 102 (go-anywhere L-1, "bus moments"): the WHEN self test (gate lane moments_when). No game, no game data: the fixtures are lines of
our own event-bus format (tools/goto/fixtures/when_events_70NN.jsonl, cut from the kept runs 7060 / 7063 / 7055; when_log_*.txt, five or six
lines of the run logs) and streams written here.

    py -I tools/goto/when_selftest.py

1. The grammar: clauses round-trip; malformed ones are refused with the reason; the address layer (match: / moment:) carries them.
2. The kept streams: @pin / @count:N / @finish{winner=} / offsets / #K / chained clauses give the frames GOANY_MOMENTS.md 3.2 pins (15081, 15168, 15158,
   15227 on run 7060); an absent event is an error that lists what the stream holds.
3. Synthetic streams for what the kept runs lack: @kickout:N~late (stopped_f), @eliminated#K, @remaining:N, @load, @code, @playing, same-frame order.
4. The live wait: a hit, a terminal event first (the finish), the budget, the vanished game.
5. The library: add --events / index / find / frame; moment:NAME@WHEN and match:SPEC@WHEN plans; the event track (a real divergence and its twin).
6. The past bug re-found (check B, run 7055): the victory arm acknowledged after `load WINNING00.PAC`: the DO-order check says "armed after the
   trigger"; run 7063's order (arm first) and a 7055 twin with the arm line moved before the load pass.
7. The game side: src/event_bus.h's kickout line carries stopped_f = g_kickout.frame.
8. PLANTED TWINS: nine faults written into copies of the sources (a text replacement that must hit exactly once), each of which the probes above
   must catch (occurrence 0-based, ~late from the emitted frame, the terminal set ignored, the offset sign flipped, remaining from the wrong
   total, search not after the previous clause, the event track skipping a missing line, the ~late threshold, the order check's direction).
Exit 0 = all passed.
"""
import json
import os
import sys
import tempfile
import types

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
FIX = os.path.join(HERE, 'fixtures')
sys.path.insert(0, HERE)
import address  # noqa: E402
import moments  # noqa: E402
import resolve  # noqa: E402
import when as W  # noqa: E402

problems = []


def check(ok, what):
    if not ok:
        problems.append(what)
        print('FAIL: ' + what)
    return ok


def ev(f, kind, **kw):
    d = {'f': f, 't': round(f / 60.0, 3), 'kind': kind}
    d.update(kw)
    return d


def m(f, what, **kw):
    return ev(f, 'match', what=what, **kw)


def write(path, text):
    with open(path, 'w', encoding='utf-8', newline='') as f:
        f.write(text)


def jsonl(events):
    return ''.join(json.dumps(e, separators=(', ', ': ')) + '\n' for e in events)


def err(fn, *a, **k):
    try:
        fn(*a, **k)
    except (W.WhenError, moments.MomentError, address.AddressError) as x:
        return str(x)
    return None


def pin_stream(stopped_after=52, at=2, with_stopped=True, interval=58):
    """a pin with counts 1 and 2, then a kickout that was emitted 20 frames after the count stopped (src/event_bus.h kKickoutHold)."""
    c1 = 1000 + 29
    c2 = c1 + interval
    stop = (c2 if at == 2 else c1) + stopped_after
    k = m(stop + 20, 'kickout', at_count=at)
    if with_stopped:
        k['stopped_f'] = stop
    return [ev(10, 'seq', seq=9, **{'from': 75}), m(100, 'clock_started', clock=1), m(1000, 'pin', pinning_slot=1), m(c1, 'count', count=1),
            m(c2, 'count', count=2), k]


def elim_stream(n=8, entrants=10):
    out = [ev(10, 'seq', seq=9, **{'from': 75}), m(100, 'clock_started', clock=1)]
    for i in range(n):
        out.append(m(2000 + 300 * i, 'eliminated', slot=i % 6, id=100 + i, over_the_top=bool(i % 2)))
    return out


def stream7060():
    return W.load_events(os.path.join(FIX, 'when_events_7060.jsonl'))


# ---------------------------------------------------------------- probes (also run against the planted twins)

def probe_core(M):
    """True when module M (when.py or a mutant of it) answers the pinned cases; any exception counts as a failure."""
    try:
        s = stream7060()
        ok = M.evaluate(s, '@pin').frame == 15081 and M.evaluate(s, '@count:2').frame == 15168 and M.evaluate(s, '@count:2-10f').frame == 15158
        ok = ok and M.evaluate(s, '@finish:pinfall').frame == 15227 and M.evaluate(s, '@pin@count:2+60f').frame == 15228
        ok = ok and M.evaluate(s, '@bell#1').frame == 12044
        two = [m(100, 'pin', pinning_slot=0), m(300, 'pin', pinning_slot=1), m(500, 'pin', pinning_slot=1)]
        two = [ev(1, 'seq', seq=9), m(100, 'pin', pinning_slot=0), m(300, 'pin', pinning_slot=1), m(500, 'pin', pinning_slot=1)]
        ok = ok and M.evaluate(two, '@pin#2').frame == 300 and M.evaluate(two, '@pin#2{by=1}').frame == 500
        # each clause searches AFTER the previous one's event: no pin after the finish
        try:
            M.evaluate(s, '@finish@pin')
            return False
        except M.WhenError:
            pass
        e = elim_stream()
        ok = ok and M.evaluate(e, '@remaining:2{of=10}').frame == 2000 + 300 * 7
        return bool(ok)
    except Exception:
        return False


def rumble_stream(burst=True):
    """the bus's Rumble lines as src/event_bus_rumble.h emits them (a 10-man: 2 starters, 8 more enter), preceded by the old start burst."""
    out = [ev(10, 'seq', seq=9, **{'from': 75})]
    if burst:
        out += [m(50, 'eliminated', slot=s, id=255, over_the_top=False) for s in (2, 3, 4)]
    out += [m(60, 'eliminated', slot=0, id=77, over_the_top=False)]  # before the bell
    out += [m(100, 'clock_started', clock=1), m(100, 'entered', n=1, slot=0, id=-1, starter=True), m(100, 'entered', n=2, slot=1, id=-1, starter=True)]
    out.append(m(150, 'eliminated', slot=4, id=255, over_the_top=False))  # after the bell, a non-wrestler seat
    out += [m(400, 'entered', n=3, slot=2, id=-1), m(405, 'entered_id', n=3, slot=2, id=343)]
    for k in range(1, 9):
        out.append(m(500 + 100 * k, 'eliminated', n=k, slot=k % 6, id=300 + k, over_the_top=True, by_idx=-1, victim_idx=k, remaining=10 - k))
    out.append(ev(2000, 'move', slot=0, id=343, waza=17, move=50413, **{'class': 'finisher'}))
    out.append(ev(2100, 'move', slot=1, id=343, waza=3, move=100, **{'class': 'normal'}))
    out.append(ev(2200, 'move', slot=1, id=139, waza=16, move=60001, **{'class': 'signature'}))
    out.append(ev(2300, 'move', slot=2, id=100, waza=18, move=51000, **{'class': 'finisher'}))
    return out


def probe_rumble(M):
    """True when module M answers the Rumble cases; any exception counts as a failure."""
    try:
        s = rumble_stream()
        ok = M.evaluate(s, '@remaining:2').frame == 1300            # no {of=N}: the eliminated line carries remaining == 2 (the 8th)
        ok = ok and M.evaluate(s, '@remaining:9').frame == 600
        ok = ok and M.evaluate(s, '@eliminated#1').frame == 600     # the 3 id-255 lines before the bell are not the first elimination
        ok = ok and M.evaluate(s, '@eliminated#8').frame == 1300
        try:
            M.evaluate(s, '@eliminated#9')
            return False
        except M.WhenError:
            pass
        ok = ok and M.evaluate(s, '@entered#3').frame == 400 and M.evaluate(s, '@entered#1').frame == 100
        ok = ok and M.evaluate(s, '@move:brock:finisher').frame == 2000 and M.evaluate(s, '@move:343:normal').frame == 2100
        ok = ok and M.evaluate(s, '@move:139:signature').frame == 2200
        try:
            M.evaluate(s, '@move:343:signature')
            return False
        except M.WhenError:
            pass
        # whole words: `rock` is The Rock (id 100), never Brock (343 / 362); `brock` reaches both Brock Lesnars; `big` is ambiguous
        ok = ok and M.evaluate(s, '@move:rock:finisher').frame == 2300 and M.evaluate(s, '@move:brock:finisher').frame == 2000
        ok = ok and M.evaluate(s, '@move:cena:signature').frame == 2200
        for bad in ('@move:big:finisher', '@move:zzzz:finisher'):
            try:
                M.parse(bad)
                return False
            except M.WhenError:
                pass
        try:
            M.evaluate([x for x in s if x.get('id') != 100], '@move:rock:finisher')
            return False
        except M.WhenError:
            pass
        # a burst-only stream (the kept run 6054 shape) has no real elimination
        k = M.load_events(os.path.join(HERE, '..', 'bus', 'fixtures', 'rumble_events_6054.jsonl'))
        try:
            M.evaluate(k, '@eliminated')
            return False
        except M.WhenError:
            pass
        return bool(ok)
    except Exception:
        return False


def probe_late(M):
    try:
        a = M.evaluate(pin_stream(52), '@kickout:2~late')
        b = M.evaluate(pin_stream(40), '@kickout:2')
        try:
            M.evaluate(pin_stream(40), '@kickout:2~late')
            return False
        except M.WhenError:
            pass
        return a.frame == 1087 + 52 + 20 and b.frame == 1087 + 40 + 20 and M.evaluate(pin_stream(53), '@kickout~late').event_frame == 1087 + 53 + 20
    except Exception:
        return False


def probe_terminal(M):
    try:
        s = [ev(1, 'seq', seq=9), m(100, 'clock_started', clock=1), m(500, 'finish', finish_type=1, finish_name='pinfall', winner_slot=0)]
        st, what = M.live_state(s, '@pin')
        st2, _ = M.live_state(s, '@finish')
        st3, _ = M.live_state(s[:2], '@pin')
        return st == 'terminal' and what['f'] == 500 and st2 == 'hit' and st3 == 'wait'
    except Exception:
        return False


def probe_order(M, late_lines, ok_lines):
    try:
        a, _ = M.order_check(late_lines, r'victory: armed scene', '@load:WINNING')
        b, _ = M.order_check(ok_lines, r'victory: armed scene', '@load:WINNING')
        return (not a) and b
    except Exception:
        return False


def probe_track(B):
    try:
        ref = [e for e in stream7060() if e['f'] >= 12000]
        same = B.event_track(ref, [dict(e, t=9.9) for e in ref], 16000, complete=True).ok
        missing = B.event_track(ref, [e for e in ref if not (e['kind'] == 'match' and e.get('what') == 'count' and e['count'] == 2)], 16000, complete=True)
        return same and (not missing.ok) and missing.first_diff == 15168
    except Exception:
        return False


def mutant(modname, old, new):
    """a copy of tools/goto/<modname>.py with `old` replaced by `new` (must occur once), as a module. Raises when the text is not there:
    a twin that changes nothing proves nothing."""
    path = os.path.join(HERE, modname + '.py')
    src = open(path, encoding='utf-8').read().replace('\r\n', '\n')
    assert src.count(old) == 1, 'planted fault text not found exactly once in %s: %r (%d)' % (modname, old[:60], src.count(old))
    mod = types.ModuleType(modname + '_mutant')
    mod.__file__ = path
    exec(compile(src.replace(old, new), modname + '_mutant', 'exec'), mod.__dict__)
    return mod


def main():
    lib = tempfile.mkdtemp(prefix='l102_moments_')
    os.environ['RDR_MOMENTS_DIR'] = lib
    s7060, s7063, s7055 = stream7060(), W.load_events(os.path.join(FIX, 'when_events_7063.jsonl')), W.load_events(os.path.join(FIX, 'when_events_7055.jsonl'))

    # ---- 1. the grammar
    for t, c in [('@pin', '@pin'), ('@count:2-10f', '@count:2-10f'), ('@kickout:2~late-30f', '@kickout:2~late-30f'), ('@kickout~late', '@kickout~late'),
                 ('@pin#2{by=1}', '@pin#2{by=1}'), ('@pin{by=1}#2', '@pin#2{by=1}'), ('@finish:pinfall{winner=Daniel Bryan}', '@finish:pinfall{winner=Daniel Bryan}'),
                 ('@eliminated#3', '@eliminated#3'), ('@remaining:2{of=10}+1s', '@remaining:2{of=10}+60f'), ('@load:WINNING-5s', '@load:WINNING-300f'),
                 ('@code:X01', '@code:X01'), ('@playing:1090', '@playing:1090'), ('@entrance:1:5', '@entrance:1:5'), ('@bell@pin@count:3', '@bell@pin@count:3'),
                 ('@seq:9', '@seq:9')]:
        check(not err(W.parse, t) and W.canon(t) == c, 'WHEN %r should canonicalise to %r (got %r)' % (t, c, err(W.parse, t) or W.canon(t)))
    for bad in ['', 'pin', '@', '@bogus', '@pin:3', '@count', '@count:4', '@count:x', '@pin#0', '@pin~late', '@remaining:0', '@load', '@kickout:x', '@pin{by}',
                '@pin{=1}', '@pin#1#2', '@code:Q', '@entrance:x', '@pin+5', '@bell:1', '@finish:pin!']:
        check(err(W.parse, bad) is not None, 'malformed WHEN %r must be refused' % bad)
    A = address.parse
    check(str(A('match:Daniel Bryan vs Brock Lesnar; players=0@pin-60f')) == 'match:Daniel Bryan vs Brock Lesnar; players=0@pin-60f' and
          A('match:Daniel Bryan vs Brock Lesnar; players=0@pin-60f').when == ('@pin-60f',), 'match: carries a WHEN with its offset')
    check(A('match:X vs Y@bell@kickout:2~late').when == ('@bell', '@kickout:2~late'), 'a WHEN keeps the order written (and takes @bell along)')
    a = A('match:X vs Y@bell')
    check(a.bell and not a.when and A('match:X vs Y@entrance:1').entrance == 1, 'a lone @bell / @entrance keeps the old live step (lane 31/N1 behaviour unchanged)')
    check(err(A, 'match:X vs Y@pin@after:5') and err(A, 'match:X vs Y@victory:5@count:1'), 'a WHEN cannot be combined with @after / @victory')
    check(A('moment:fx@count:2-10f').when == ('@count:2-10f',) and str(A('moment:fx@count:2-10f')) == 'moment:fx@count:2-10f', 'moment: carries a WHEN')
    check(A('moment:fx@frame:30').frame == 30 and not A('moment:fx@frame:30').when, 'moment:@frame:F is unchanged')
    check(err(A, 'moment:fx@bogus:1') and err(A, 'moment:fx@count:9'), 'a malformed moment: WHEN is refused')

    # ---- 2. the kept streams (GOANY_MOMENTS.md 3.2)
    exp = [('@pin', 15081), ('@count:1', 15110), ('@count:2', 15168), ('@count:2-10f', 15158), ('@count:2+1s', 15228), ('@finish:pinfall{winner=Daniel Bryan}', 15227),
           ('@finish{loser=Brock Lesnar}', 15227), ('@finish:pinfall{winner=324}', 15227), ('@bell', 12044), ('@seq:9', 3300), ('@pin@count:2', 15168),
           ('@pin@finish', 15227), ('@load:WINNING', None)]
    for t, want in exp:
        e = err(W.evaluate, s7060, t)
        if want is None:
            check(e is not None and 'The stream holds: bell at 12044; pins at 15081; kickouts: none; eliminated: none; finish pinfall at 15227' in e, '7060 %s must be absent, listing the stream: %s' % (t, e))
        else:
            h = None if e else W.evaluate(s7060, t)
            check(h is not None and h.frame == want, '7060 %s should name frame %d (got %s)' % (t, want, e or h.frame))
    check(W.evaluate(s7060, '@count:2-10f').needs_recording and not W.evaluate(s7060, '@count:2+10f').needs_recording, 'a negative offset needs a recording, a positive one does not')
    e = err(W.evaluate, s7060, '@pin#2')
    check(e and 'pins at 15081' in e and '#2 asked' in e, '@pin#2 on one pin: an error listing the one pin: %s' % e)
    e = err(W.evaluate, s7060, '@kickout')
    check(e and 'kickouts: none' in e and 'finish pinfall' in e, '@kickout on a stream with none: %s' % e)
    check(err(W.evaluate, s7060, '@finish{winner=Brock Lesnar}') is not None, 'the winner filter distinguishes the wrestlers')
    check(err(W.evaluate, s7060, '@finish{winner=Nobody At All}') and 'not in db/ids/characters.json' in err(W.evaluate, s7060, '@finish{winner=Nobody At All}'), 'an unknown name is refused, not matched to nothing')
    check(W.evaluate(s7060, '@pin{by=1}').frame == 15081 and err(W.evaluate, s7060, '@pin{by=0}') is not None, 'by= filters on the pinning slot')
    check(W.evaluate(s7063, '@pin').frame == 16445 and W.evaluate(s7055, '@pin').frame == 16564 and W.evaluate(s7055, '@finish{winner=Brock Lesnar}').frame == 16710 and
          W.evaluate(s7055, '@load:WINNING').frame == 18176 and W.evaluate(s7055, '@finish@load:WINNING').frame == 18176, 'runs 7063 / 7055 give their own frames (7055: Lesnar won; WINNING00 opens at 18176)')
    check(err(W.evaluate, s7060, '@pin-15100f') is not None, 'an offset before the first frame is refused')

    # ---- 3. synthetic streams
    check(probe_late(W), '@kickout:2~late: stopped 52 frames after count 2 (interval 58, threshold round(0.9 x 58) = 52) is late, 40 is not, from stopped_f')
    e = err(W.evaluate, pin_stream(52, with_stopped=False), '@kickout:2~late')
    check(e and 'stopped_f' in e and 'older build' in e, 'an old-build kickout (no stopped_f) cannot be judged late: %s' % e)
    check(W.evaluate(pin_stream(52, with_stopped=False), '@kickout:2').frame == 1087 + 72, 'without ~late an old kickout still resolves (to its emitted frame)')
    info = W.late_info(pin_stream(52), 5)
    check(info == (1139, 1087, 58, 52, True), 'late_info of a 52-frame wait: %s' % (info,))
    st1 = pin_stream(10, at=1)
    check(W.late_info(st1, 5)[2] == W.DEFAULT_COUNT_INTERVAL or W.late_info(st1, 5)[2] == 58, 'a kickout at count 1 (one count in the pin) uses the measured default interval')
    check(W.evaluate(elim_stream(), '@eliminated#3').frame == 2600 and W.evaluate(elim_stream(), '@eliminated{slot=0}#2').frame == 2000 + 300 * 6,
          '@eliminated#K counts the filtered eliminations')
    check(W.evaluate(elim_stream(), '@eliminated{over_the_top=true}#1').frame == 2300, 'a boolean filter reads true / false')
    check(W.evaluate(elim_stream(8), '@remaining:2{of=10}').frame == 2000 + 300 * 7, '@remaining:2 of 10 entrants = the 8th elimination')
    check(err(W.evaluate, elim_stream(7), '@remaining:2{of=10}') is not None, '@remaining:2 with only 7 eliminations is not found')
    check('needs the entrant count' in (err(W.evaluate, elim_stream(), '@remaining:2') or '') and W.evaluate(elim_stream(), '@remaining:2', entrants=10).frame == 4100,
          '@remaining needs the entrant count, from {of=N} or the caller')
    check(err(W.evaluate, elim_stream(), '@remaining:10{of=10}') is not None, '@remaining:N with N = the entrants is the bell, not an elimination')
    wm = [ev(5, 'seq', seq=9), ev(700, 'wm30', code=303), ev(900, 'wm30', code=301), ev(1200, 'scene', bank=1090), ev(1300, 'load', file='D:\\PAC\\EVT\\Winning00.pac')]
    check(W.evaluate(wm, '@code:X01').frame == 900 and W.evaluate(wm, '@code:303').frame == 700 and W.evaluate(wm, '@code:X03').frame == 700 and W.evaluate(wm, '@playing:1090').frame == 1200 and
          W.evaluate(wm, '@load:winning').frame == 1300 and err(W.evaluate, wm, '@playing:1091') is not None, '@code / @playing / @load (case-insensitive) on synthetic bus kinds')
    same = [m(500, 'count', count=1), m(500, 'pin', pinning_slot=1), m(500, 'count', count=2)]
    check(W.evaluate(same, '@pin@count:2').frame == 500 and err(W.evaluate, same, '@pin@count:1') is not None, 'same-frame lines: a clause searches after the previous one by stream position')
    check(probe_core(W), 'probe_core on the real module')
    # S101 lane 120: the Rumble bus lines
    check(probe_rumble(W), 'probe_rumble on the real module: @remaining reads the event, @entered#K, @move, the id-255 burst is not counted')
    check(probe_terminal(W), 'probe_terminal on the real module')

    # ---- 4. the live wait
    class FakeEvents:
        def __init__(self, batches):
            self.b = list(batches)

        def read(self):
            return self.b.pop(0) if self.b else []

    class NoGame(Exception):
        pass

    fake = types.ModuleType('live')
    fake.NoGame = NoGame
    fake.calls = []
    fake.request = lambda name, text, connect_wait=1.0: fake.calls.append(text) or ''
    fake.run_batch = lambda name, lines, timeout=60: fake.calls.append(lines)
    sys.modules['live'] = fake
    resolve.time.sleep = lambda x: None
    base = {'when': '@pin', 'terminal': ['finish', 'seq leaving 9'], 'budget_frames': 5000, 'timeout_s': 5, 'entrants': None}

    def wait(batches, **over):
        step = dict(base, **over)
        try:
            return resolve._wait_when(step, 'main', FakeEvents(batches), lambda t: None), None
        except resolve.Stopped as x:
            return None, str(x)
    pin_ev = [ev(5, 'seq', seq=9), m(100, 'clock_started', clock=1)], [m(900, 'pin', pinning_slot=1)]
    h, e = wait(pin_ev)
    check(h is not None and h.frame == 900, 'wait_when arrives on the pin line: %s' % e)
    h, e = wait(pin_ev, when='@pin+30f')
    check(h is not None and ['wait_frames 30'] in fake.calls, 'a positive offset waits that many more frames after the line')
    h, e = wait(([ev(5, 'seq', seq=9), m(100, 'clock_started', clock=1)], [m(500, 'finish', finish_type=1, finish_name='pinfall', winner_slot=0)]))
    check(h is None and e and 'before the match moved on' in e and 'finish pinfall at 500' in e, 'the finish before the pin is a terminal stop, loudly: %s' % e)
    h, e = wait(([ev(5, 'seq', seq=9), ev(4000, 'seq', seq=75, **{'from': 9})],))
    check(h is None and e and 'moved on' in e, 'leaving sequence 9 is a terminal stop: %s' % e)
    h, e = wait(([ev(5, 'seq', seq=9), m(100, 'clock_started', clock=1), ev(9000, 'load', file='x.pac')],))
    check(h is None and e and 'within 5000 guest frames' in e, 'the frame budget ends the wait: %s' % e)
    h, e = wait(([ev(5, 'seq', seq=9), m(100, 'finish', finish_type=1, finish_name='pinfall', winner_slot=0)],), when='@finish', terminal=[])
    check(h is not None and h.frame == 100, 'the finish is not terminal for a @finish wait')
    h, e = wait(([ev(5, 'seq', seq=9), m(100, 'finish', finish_type=1, finish_name='pinfall', winner_slot=0)],), when='@finish@load:WINNING', terminal=[])
    check(h is None and e is not None and 'did not happen' in e, 'a finish matched by an earlier clause is not a terminal for the next clause (timeout, not terminal)')
    check(probe_terminal(W) and not probe_terminal(mutant('when', "if 'finish' not in left and _is(e, 'match', 'finish'):", "if False and _is(e, 'match', 'finish'):")),
          'twin: the terminal set ignored is caught')

    # ---- 5. the library
    rec_text = '# wwe2k14 replay v2 (src/replay.h): synthetic fixture of tools/goto/when_selftest.py\nname fx\nseed 77\ndlc none\nflag det_frames true\n' + \
        ''.join('T %d %d\n' % (f, f * 3) for f in range(60, 15601, 60))
    rec_path = os.path.join(lib, '_fx.rec')
    write(rec_path, rec_text)
    ev7060 = os.path.join(FIX, 'when_events_7060.jsonl')
    place = 'Daniel Bryan vs Brock Lesnar; players=0; pace=quick'
    meta = moments.add_moment('bryan_lesnar', rec_path, shows='fixture', events=ev7060, place=place)
    check(meta['place'] == place and meta['index']['counts']['match:pin'] == 1 and meta['bus_events'] == 'events.jsonl' and not moments.check_moment('bryan_lesnar'),
          'add --events writes the index and the moment checks clean: %s' % moments.check_moment('bryan_lesnar'))
    f, hit, _, _ = moments.when_frame('bryan_lesnar', '@pin-60f')
    check(f == 15021 and hit.event_frame == 15081, 'moment_when @pin-60f = 15021 (event 15081)')
    check(moments.when_frame('bryan_lesnar', '@finish:pinfall{winner=Daniel Bryan}')[0] == 15227, 'moment WHEN with a name filter')
    for bad, why in [('@pin#2', 'pins at 15081'), ('@kickout', 'kickouts: none'), ('@pin+9999f', 'past the recording')]:
        e = err(moments.when_frame, 'bryan_lesnar', bad)
        check(e is not None and why in e, 'moment WHEN %s must fail loudly (%s): %s' % (bad, why, e))
    p = resolve.resolve('moment:bryan_lesnar@pin-60f')
    check(p.status == 'plan' and p.steps[0]['until'] == 15021 and p.steps[1]['until'] == 15021 and p.steps[0]['events'].endswith('events.jsonl') and
          any('event track' in n for n in p.notes) and 'bus lines' in resolve.describe_step(p.steps[1]), 'moment:@pin-60f plans the hand-over at 15021 with the event track: %s' % p.lines())
    check('--wwe2k14_events=true' in resolve.moment_flags(p.steps[0]) and '--wwe2k14_replay_until=15021' in resolve.moment_flags(p.steps[0]), 'moment_start asks for the bus')
    p = resolve.resolve('moment:bryan_lesnar@kickout')
    check(p.status == 'error' and 'kickouts: none' in p.reason, 'moment:@kickout is an ERROR plan listing the index')
    p = resolve.resolve('moment:nosuch@pin')
    check(p.status == 'error', 'an unknown moment is an ERROR plan')
    write(os.path.join(lib, '_plain.rec'), rec_text)
    moments.add_moment('plain', os.path.join(lib, '_plain.rec'))
    p = resolve.resolve('moment:plain@pin')
    check(p.status == 'error' and 'no event index' in p.reason, 'a moment without an index: the WHEN says how to index it: %s' % p.reason)
    check(resolve.resolve('moment:plain@frame:300').status == 'plan', 'a moment without an index still plays @frame:F')
    hits, skipped = moments.find_moments('@pin')
    check([(h[0], h[1]) for h in hits] == [(15081, 'bryan_lesnar')] and [n for n, _ in skipped] == ['plain'], 'find: the indexed moment, the plain one listed as skipped: %s %s' % (hits, skipped))
    check(moments.find_moments('@pin', place='match:Daniel Bryan vs Brock Lesnar; pace=quick; players=0')[0] and
          not moments.find_moments('@pin', place='Daniel Bryan vs Brock Lesnar; players=0; pace=normal')[0], 'find: the place filter compares the spec (field order free, values exact)')
    check(not moments.find_moments('@kickout')[0], 'find: no moment, no hit')
    # plans of a match address
    p = resolve.resolve('match:Daniel Bryan vs Brock Lesnar; players=0; pace=quick@pin-60f')
    check(p.status == 'plan' and p.address.endswith('@pin-60f') and p.steps[0]['until'] == 15021 and any(n.startswith('recorded: moment "bryan_lesnar"') for n in p.notes),
          'match:SPEC@pin-60f with a recording of the same place plans the recorded route: %s' % p.lines()[:3])
    p = resolve.resolve('match:Daniel Bryan vs Brock Lesnar; players=0; pace=normal@pin-60f')
    check(p.status == 'error' and 'needs a recording' in p.reason and 'run the harvest' in p.reason, 'a negative offset without a recording of the place is an error naming the harvest: %s' % p.reason)
    p = resolve.resolve('match:Daniel Bryan vs Brock Lesnar; players=0; pace=normal@pin')
    ops = [s['op'] for s in p.steps]
    check(p.status == 'plan' and ops == ['events_mark', 'nav', 'match', 'wait_when'] and p.steps[3]['terminal'] == ['finish', 'seq leaving 9'] and p.steps[3]['budget_frames'] == resolve.LIVE_BUDGET_FRAMES,
          'match:SPEC@pin (no recording) = the builder, then a live wait with its terminal set and budget: %s' % p.lines())
    p = resolve.resolve('match:Daniel Bryan vs Brock Lesnar; players=0; pace=quick@pin', resolve.Context(route='live'))
    check(p.status == 'plan' and p.steps[-1]['op'] == 'wait_when', '--live forces the live route although a recording has it')
    p = resolve.resolve('match:Daniel Bryan vs Brock Lesnar; players=0; pace=normal@pin', resolve.Context(route='recorded'))
    check(p.status == 'error' and '--recorded' in p.reason, '--recorded without a recording is an error')
    p = resolve.resolve('match:Daniel Bryan vs Brock Lesnar; players=0@finish@load:WINNING')
    check(p.status == 'plan' and p.steps[-1]['terminal'] == ['seq leaving 9'], 'a @finish clause removes the finish from the terminal set')
    p = resolve.resolve('match:royal rumble;type=royal rumble > 10;rumble=10 random;players=0@remaining:2')
    check(p.status in ('plan', 'error') and (p.status == 'error' or (p.steps[-1]['entrants'] == 10 and p.steps[-1]['budget_frames'] == resolve.RUMBLE_BUDGET_FRAMES)),
          'a Rumble spec gets the long budget and rumble=N as the entrant count (if the atlas builds it): %s' % p.lines()[:2])
    p = resolve.resolve('match:Daniel Bryan vs Brock Lesnar; players=0@remaining:2')
    check(p.status == 'error' and '{of=N}' in p.reason, '@remaining without an entrant count is refused at plan time for a non-Rumble spec')
    p = resolve.resolve('match:royal rumble;type=royal rumble > 10;players=0@remaining:2')
    check(p.status != 'error' or '{of=N}' not in p.reason, 'a Rumble spec needs no {of=N} for @remaining (the bus carries it): %s' % p.lines()[:2])
    # indexing refusals
    short = os.path.join(lib, '_short.jsonl')
    write(short, jsonl([ev(1, 'seq', seq=3), ev(300, 'seq', seq=75)]))
    check('shorter run' in (err(moments.add_moment, 'short1', rec_path, events=short) or '') and not os.path.exists(os.path.join(lib, 'short1')), 'a bus stream that stops 15000 frames early is refused, and nothing is left behind')
    check(moments.add_moment('short1', rec_path, events=short, allow_short=True)['bus_events'] == 'events.jsonl', '--allow-short registers it')
    write(short, '')
    check('empty' in (err(moments.add_moment, 'short2', rec_path, events=short) or ''), 'an empty stream is refused')
    write(short, jsonl([m(100, 'pin', pinning_slot=0), m(50, 'count', count=1)]))
    check(err(moments.add_moment, 'short3', rec_path, events=short, allow_short=True) is not None, 'a stream with no seq line / going back in time is refused')
    write(short, '{"f": 1, "kind": "seq", "seq": 3}\nnot json\n')
    check('not JSON' in (err(moments.add_moment, 'short4', rec_path, events=short) or ''), 'a corrupt stream line is named')
    check(err(W.load_events, os.path.join(lib, 'nope.jsonl')) is not None, 'a missing stream file is an error, not an empty stream')
    # tamper
    bp = os.path.join(lib, 'bryan_lesnar', 'events.jsonl')
    good = open(bp, encoding='utf-8', newline='').read()
    write(bp, good.replace('"count": 2', '"count": 3', 1))
    check('changed since it was indexed' in (err(moments.when_frame, 'bryan_lesnar', '@pin') or '') and moments.check_moment('bryan_lesnar'), 'a changed events.jsonl is caught (when_frame and check)')
    write(bp, good)
    check(not moments.check_moment('bryan_lesnar'), 'restored: clean again')

    # ---- 5b. the event track
    ref = [e for e in s7060 if e['f'] >= 12000]
    run = [dict(e, t=e['t'] + 7.0) for e in ref]    # wall seconds differ by run: not compared
    T = moments.bus.event_track
    t = T(ref, run, 15200, complete=True)
    check(t.ok and t.compared >= 3, 'the event track passes an identical replay (t differs): %s' % t.line())
    shifted = [dict(e, f=e['f'] + 1) if (e['kind'] == 'match' and e.get('what') == 'count' and e.get('count') == 2) else e for e in run]
    t = T(ref, shifted, 15200, complete=True)
    check(not t.ok and t.first_diff == 15168 and 'missing from the replay' in t.reason, 're-found (S77 nowalk2679 style): the bus parts at the first differing line, frame 15168: %s' % t.line())
    t = T(ref, [e for e in run if e['f'] < 15100], 15200, complete=True)
    check(not t.ok and t.first_diff == 15110, 'complete: a replay that stops early has missing lines: %s' % t.line())
    t = T(ref, [e for e in run if e['f'] < 15100], 15200, complete=False)
    check(t.ok, 'in progress: only the frames before the run\'s last line are judged')
    t = T(ref, run + [m(14000, 'count', count=1)], 15200, complete=True)
    check(not t.ok and 'not in the index' in t.reason, 'an extra line in the replay stops the track')
    t = T(ref, run + [m(15300, 'count', count=1)], 15200, complete=True)
    check(t.ok, 'lines at or after the hand-over frame are not judged')
    t = T(ref, [], 15200, complete=True)
    check(not t.ok and 'no bus lines' in t.reason, 'a replay with no bus file lines fails, not passes')
    t = T(ref + [ev(15000, 'state', clock=5)], run, 15200, complete=True)
    check(t.ok, 'state lines (on request) are not part of the track')
    check(probe_track(moments.bus), 'probe_track on the real module')

    # ---- 5c. the event track inside moment_wait (fake live channel; the replay's bus file is written by the fake game)
    resolve.POLL_S, resolve.FINAL_WAIT_S = 0.01, 0.3

    _ng = NoGame

    class FakeLive:
        NoGame = _ng

        def __init__(self, base, bus_lines):
            self.REPO = self.EXE_DIR = base
            self.log, self.n, self.bus_lines, self.batches = os.path.join(base, 'run.log'), 0, bus_lines, []
            write(self.log, '')

        def parse(self, text):
            return {'run': '7'}, ''

        def run_log(self, run):
            return self.log

        def request(self, name, text, connect_wait=1.0):
            self.n += 1
            if self.n == 1:
                with open(self.log, 'a') as f:
                    f.write('[wwe2k14][replay] replay_until: hand over at frame 15200 (recording ends at 15600)\n[wwe2k14][replay] playing x.rec (1 lines)\n')
                if self.bus_lines is not None:
                    write(os.path.join(os.path.dirname(self.log), 'events_007.jsonl'), jsonl(self.bus_lines))
            if self.n == 3:
                with open(self.log, 'a') as f:
                    f.write('[wwe2k14][replay] recording stopped (replay_until) at frame 15200: the script / real controller take over.\n')
            return 'ok'

        def run_batch(self, name, lines, timeout=120, **k):
            self.batches.append(lines)
            return {}, '', []

        def bookmark(self, name):
            return 'bookmark taken', None

    bus_ref = os.path.join(lib, 'bryan_lesnar', 'events.jsonl')

    def run_track(bus_lines):
        fl = FakeLive(tempfile.mkdtemp(prefix='l102_fakelive_', dir=lib), bus_lines)
        sys.modules['live'] = fl
        try:
            resolve._moment_wait({'name': 'fx', 'until': 15200, 'det': None, 'timeout_s': 20, 'events': bus_ref}, 'main', lambda *_: None)
            return None
        except resolve.Stopped as x:
            return str(x)
        finally:
            del sys.modules['live']
    good = [dict(e, t=e['t'] + 3.0) for e in W.load_events(bus_ref) if e['f'] < 15200]
    check(run_track(good) is None, 'moment_wait with the event track: an identical replay hands over cleanly: %s' % run_track(good))
    bad = [dict(e, f=e['f'] + 1) if (e['kind'] == 'match' and e.get('count') == 2) else e for e in good]
    e = run_track(bad)
    check(e is not None and 'event track STOPPED at frame 15168' in e, 'moment_wait stops at the first differing bus line: %s' % e)
    e = run_track([x for x in good if x['f'] < 15100])
    check(e is not None and 'event track STOPPED' in e, 'a replay whose bus stops early cannot hand over (missing lines at the end are a stop): %s' % e)
    e = run_track(None)
    check(e is not None and 'wrote no bus file' in e, 'a replay with no bus file cannot pass the event track: %s' % e)

    # ---- 6. the past bug re-found
    late = open(os.path.join(FIX, 'when_log_7055_victory_late.txt'), encoding='utf-8').read().split('\n')[:-1]
    okl = open(os.path.join(FIX, 'when_log_7063_victory_ok.txt'), encoding='utf-8').read().split('\n')[:-1]
    ok, msg = W.order_check(late, r'victory: armed scene', '@load:WINNING')
    check(not ok and 'armed after the trigger' in msg and 'frame 18176' in msg, 'RE-FIND run 7055 (check B): the victory arm after `load WINNING00.PAC` at frame 18176 is flagged: %s' % msg)
    ok, msg = W.order_check(okl, r'victory: armed scene', '@load:WINNING')
    check(ok and 'before the trigger' in msg, 'run 7063 (arm at frame 4141, load at 17338) passes: %s' % msg)
    arm = [l for l in late if 'victory: armed' in l]
    twin = arm + [l for l in late if 'victory: armed' not in l]
    ok, msg = W.order_check(twin, r'victory: armed scene', '@load:WINNING')
    check(ok, 'twin: the 7055 log with the arm line moved before the load passes: %s' % msg)
    ok, msg = W.order_check([l for l in late if 'victory: armed' not in l], r'victory: armed scene', '@load:WINNING')
    check(not ok and 'never acknowledged' in msg, 'no arm line at all is a failure, not a pass')
    ok, msg = W.order_check([l for l in late if 'WINNING' not in l], r'victory: armed scene', '@load:WINNING')
    check(not ok and 'not in the log' in msg, 'no trigger event in the log is a failure, not a pass')
    check(probe_order(W, late, okl), 'probe_order on the real module')

    # ---- 7. the game side
    h = open(os.path.join(REPO, 'src', 'event_bus.h'), encoding='utf-8').read().replace('\r\n', '\n')
    check('\\"at_count\\": {}, \\"stopped_f\\": {}", g_kickout.count, g_kickout.frame' in h and 'S101 lane 102' in h, 'src/event_bus.h: the kickout line carries stopped_f = g_kickout.frame (S101 lane 102)')
    i = h.index('inline void MatchTick')
    check(h.count('stopped_f') >= 2 and 'g_kickout = {frame, g_last.ref_last_count};' in h[i:], 'g_kickout.frame is the frame the referee mode left 1 (the count stopped), not the emit frame')
    check(h[:h.index('namespace')].count('stopped_f') == 1, 'the bus header documents stopped_f')

    # lane 150: G+472 = 15 (a playing 30 Years scene) is a `scene` line, never a `finish`: @finish cannot fire on it, and it is not a terminal event
    check('constexpr int64_t kSceneFinishType = 15;' in h and 'if (finish == kSceneFinishType)' in h and '\\"what\\": \\"scene\\"' in h
          and h.index('if (finish == kSceneFinishType)') < h.index('\\"what\\": \\"finish\\"'), 'src/event_bus.h: finish type 15 is emitted as what=scene (lane 150)')
    ss = [ev(1, 'seq', seq=9), m(100, 'clock_started', clock=1), m(200, 'scene', g472=15, clock=35)]
    st_a, _ = W.live_state(ss, '@pin')
    st_b, _ = W.live_state(ss, '@finish')
    check(st_a == 'wait' and st_b == 'wait', 'lane 150: a match/scene line is neither a terminal event nor a @finish hit (got %s / %s)' % (st_a, st_b))
    twin_ss = ss[:2] + [m(200, 'finish', finish_type=15, finish_name='unknown')]
    check(W.live_state(twin_ss, '@pin')[0] == 'terminal' and W.live_state(twin_ss, '@finish')[0] == 'hit', 'lane 150 planted twin: the same line as `finish` IS caught as a finish')

    # ---- 8. planted twins (each caught by the probes above)
    twins = [
        ('occurrence 0-based', 'when', 'i, e = cand[c.occ - 1]', 'i, e = cand[c.occ]', probe_core),
        ('~late from the emitted frame', 'when', "return k['stopped_f'], base, interval, threshold, (k['stopped_f'] - base) >= threshold",
         "return k['f'], base, interval, threshold, (k['f'] - base) >= threshold", probe_late),
        ('~late threshold 0.4', 'when', 'LATE_FRACTION = 0.9', 'LATE_FRACTION = 0.4', probe_late),
        ('offset sign flipped', 'when', "frame = e['f'] + c.offset", "frame = e['f'] - c.offset", probe_core),
        ('remaining from the wrong total', 'when', 'need = of - int(c.arg)\n', 'need = of - int(c.arg) + 1\n', probe_core),
    ]
    twins += [
        ('Rumble: remaining read from the wrong event', 'when', "return [(i, e) for i, e in carried if e['remaining'] == int(c.arg)][:1]",
         "return [(i, e) for i, e in carried if e['remaining'] == int(c.arg) + 1][:1]", probe_rumble),
        ('Rumble: the id-255 filter dropped', 'when', "and e.get('id') != 255   # S101 lane 120: id 255", "   # S101 lane 120: id 255", probe_rumble),
        ('Rumble: eliminations before the bell counted', 'when', "if c.name == 'eliminated' and i <= first_bell:", "if False:", probe_rumble),
        ('Rumble: @entered#K off by one', 'when', "        return _is(e, 'match', 'entered')", "        return _is(e, 'match', 'entered') and e.get('n') != 1", probe_rumble),
        ('Rumble: @move matches substrings (rock hits Brock)', 'when', "if all(w in re.findall(r'[a-z0-9]+', n.lower()) for w in want):",
         "if all(w in n.lower() for w in want):", probe_rumble),
        ('Rumble: @move ignores the class', 'when', "e.get('class') == cls and ", "", probe_rumble),
    ]
    for name, mod, old, new, probe in twins:
        try:
            mm = mutant(mod, old, new)
        except AssertionError as x:
            check(False, 'twin "%s" could not be planted: %s' % (name, x))
            continue
        check(probe(mm) is False, 'PLANTED TWIN "%s" was not caught by its probe' % name)
    try:
        mm = mutant('when', "        last = (i, e, c)\n        start = i + 1", "        last = (i, e, c)\n        start = 0")
        check(probe_core(mm) is False, 'PLANTED TWIN "search not after the previous clause" was not caught')
    except AssertionError as x:
        check(False, 'twin "search start" could not be planted: %s' % x)
    mm = mutant('moments_bus', 'if a != b:', 'if b and a != b:')
    check(probe_track(mm) is False and probe_track(moments.bus), 'PLANTED TWIN "the event track skips a missing line" was not caught')
    mm = mutant('when', 'if arm_i > trig_i:', 'if arm_i < 0:')
    check(probe_order(mm, late, okl) is False and probe_order(W, late, okl), 'PLANTED TWIN "the order check ignores the order" was not caught')

    print('when selftest: %s' % ('%d problems' % len(problems) if problems else '0 problems (all checks passed)'))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
