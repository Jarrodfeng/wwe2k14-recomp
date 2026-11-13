"""S101 lane 145: offline tests of the stall watchdog (tools/live/stall_guard.py). 0 game runs.

  py -I tools/live/stall_selftest.py

1. the policy table (safe texts press; save / delete / overwrite / unknown / glyph box outside a match STOP);
2. the J3 replay: a 1 Hz status stream rebuilt from the evidence of run 7091 (fixtures/stall_j3_run7091.json, counts only; the
   real log is cross-checked when it is present) -> the detector flags the stall and picks "press A";
3. the same stream with the dialog text swapped for a save prompt -> STOP, no press (the planted twin);
4. the press budget (3), recovery after a press, a loading wait (no text), a hung game and the clock at 0;
5. the plumbing: Guard.tick against a fake channel presses through live.run_batch; resolve's waits carry the tick.
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import stall_guard as SG  # noqa: E402

REPO = os.path.dirname(os.path.dirname(HERE))
FIX = os.path.join(HERE, 'fixtures', 'stall_j3_run7091.json')
REAL_LOG = r'C:\Users\Jarrod\wwe2k14-recomp\out\build\win-amd64-release\logs\wwe2k14_7091.log'

fails = []


def check(ok, what):
    print('%s  %s' % ('ok  ' if ok else 'FAIL', what))
    if not ok:
        fails.append(what)


def status_fields(frame, clock, text, seq=9, scene=1):
    return {'status': 'ok', 'frame': str(frame), 'states': 'seq=%d scene_word=%d match_clock=%d' % (seq, scene, clock), 'text': text}


# ---------------------------------------------------------------- 1. policy
POLICY = [
    ('??? OK', 9, 'press', 'A'),
    ('OK', 9, 'press', 'A'),
    ('Press A', 3, 'press', 'A'),
    ('Continue', 75, 'press', 'A'),
    ('Press START', 3, 'press', 'START'),
    ('PRESS START', 3, 'press', 'START'),
    ('??? | OK', 9, 'press', 'A'),
    ('Win the match in under 6 minutes | Continue', 9, 'press', 'A'),
    # stops
    ('Overwrite existing save? | OK', 9, 'stop', ''),
    ('Saving... Do not turn off | OK', 75, 'stop', ''),
    ('Delete this profile? | Yes | No', 75, 'stop', ''),
    ('Are you sure you want to quit? OK', 9, 'stop', ''),
    ('Storage device removed | OK', 75, 'stop', ''),
    ('Exit match | OK', 9, 'stop', ''),
    ('Create a Wrestler | OK', 75, 'stop', ''),       # unknown body segment
    ('??? OK', 75, 'stop', ''),                        # glyph box outside a match: could be a save notice
    ('??? OK', None, 'stop', ''),
    ('Win the match in under 6 minutes | Continue', 75, 'stop', ''),
    ('PAUSE | Resume Match | Quit Match', 9, 'stop', ''),
    ('', 9, 'stop', ''),
    ('Accept | Cancel', 9, 'stop', ''),
]
for text, seq, kind, btn in POLICY:
    d = SG.decide(text, seq)
    check(d.kind == kind and d.button == btn, "decide(%r, seq %s) -> %s %s (%s)" % (text, seq, d.kind, d.button, d.why))

# ---------------------------------------------------------------- 2. J3 replay
fx = json.load(open(FIX, encoding='utf-8'))
bell = fx['bell_frame']
per = fx['glyph_draws_per_frame']
win = [w for w in fx['census_windows'] if 0 < w['text_draws'] < per * (w['frames'][1] - w['frames'][0])][0]
box_start = win['frames'][1] - round(win['text_draws'] / per)
check(box_start == fx['expect']['box_start_frame'], 'census: window %s with %d glyph draws -> the box rose at frame %d (expected %d)'
      % (win['frames'], win['text_draws'], box_start, fx['expect']['box_start_frame']))
if os.path.exists(REAL_LOG):
    log = open(REAL_LOG, encoding='utf-8', errors='replace').read()
    check(('match clock started at frame %d' % bell) in log and ('frames %d-%d:' % tuple(win['frames'])) in log,
          'cross-check against the real run-7091 log: the bell and the census window are in it')
else:
    print('skip  (the real run-7091 log is not on this machine: the committed fixture alone is used)')
bk = fx['bookmarks'][0]


def j3_stream(text=bk['text'], step=60, start=bell - 124, until=None, recover_after=None):
    """1 Hz samples of run 7091: clock 0 before the bell, +1 a frame to 15 at the box, held; the text from the box on."""
    f = start
    out = []
    until = until or (box_start + 1200)
    while f < until:
        clock = 0 if f < bell else min(f - bell + 1, bk['match_clock'])
        shown = text if f >= box_start else ''
        if recover_after is not None and f >= recover_after:
            clock, shown = bk['match_clock'] + (f - recover_after) // 2 + 1, ''
        out.append(SG.parse_status(status_fields(f, clock, shown, bk['seq'], bk['scene_word'])))
        f += step
    return out


def replay(stream, det=None):
    det = det or SG.Detector(('match_clock',), 180, 3)
    return [(s.frame, det.feed(s)) for s in stream]


res = replay(j3_stream())
first = next(((f, v) for f, v in res if v.kind != 'ok'), None)
check(first is not None and first[1].kind == 'press' and first[1].button == 'A', 'J3 replay: first verdict is "press A"')
lag = first[0] - box_start
check(lag <= 240, 'J3 replay: flagged at frame %d = %d frames (%.1f s at 60 fps) after the box rose at %d (180 held + up to 1 s of sampling)'
      % (first[0], lag, lag / 60.0, box_start))
check(first[1].held >= 180 and first[1].value == 15 and first[1].text == '??? OK', 'J3 replay: the report line carries clock, frames and text')
print('      ' + first[1].line())
check(all(v.kind == 'ok' for f, v in res if f < box_start + 120), 'J3 replay: nothing flagged before the box / while the clock ran')

# recovery: one press brings the clock back -> no more verdicts
rec = replay(j3_stream(recover_after=first[0] + 60))
kinds = [v.kind for f, v in rec if v.kind != 'ok']
check(kinds == ['press'], 'after the press the clock runs again: exactly one press, then quiet (%s)' % kinds)

# the press budget: the box does not close -> 3 presses, then STALLED
long_res = replay(j3_stream(until=box_start + 2400))
kinds = [v.kind for f, v in long_res if v.kind != 'ok']
check(kinds[:4] == ['press', 'press', 'press', 'stop'], 'a box that stays: press x3 then stop (%s)' % kinds[:5])
stop = next(v for f, v in long_res if v.kind == 'stop')
check('still held after 3 auto-presses' in stop.line() and stop.line().startswith('STALLED: match_clock held at 15 for'), 'stop line: ' + stop.line())

# ---------------------------------------------------------------- 3. planted twin: a save prompt must STOP
twin = replay(j3_stream(text='Overwrite your saved data? | ??? | OK'))
tk = [(f, v) for f, v in twin if v.kind != 'ok']
check(bool(tk) and tk[0][1].kind == 'stop' and not any(v.kind == 'press' for f, v in twin), 'save prompt twin: first verdict is STOP and nothing is ever pressed')
print('      ' + tk[0][1].line())
twin2 = replay(j3_stream(text='Delete | OK'))
check(not any(v.kind == 'press' for f, v in twin2) and any(v.kind == 'stop' for f, v in twin2), 'delete prompt twin: stop, no press')

# ---------------------------------------------------------------- 4. other shapes
load = [SG.parse_status(status_fields(1000 + 60 * i, 15, '')) for i in range(10)]
lv = [v.kind for _, v in replay(load)]
check(set(lv) <= {'ok', 'held'} and 'held' in lv, 'held clock with no text on screen (a load): never a press, reported "held"')
frozen = [SG.parse_status(status_fields(5000, 15, '??? OK')) for _ in range(10)]
check(all(v.kind == 'ok' for _, v in replay(frozen)), 'frames not advancing (a hung game): the detector does not press')
menu = [SG.parse_status(status_fields(100 + 60 * i, 0, 'Continue', seq=75)) for i in range(10)]
check(all(v.kind == 'ok' for _, v in replay(menu)), 'clock 0 (menus / loads, no bell yet): untracked, never pressed')
running = [SG.parse_status(status_fields(100 + 60 * i, 20 + 60 * i, 'OK')) for i in range(10)]
check(all(v.kind == 'ok' for _, v in replay(running)), 'a running clock with text: not a stall')
check(SG.parse_status({'frame': '12', 'states': 'seq=9 menu=unreadable match_clock=7', 'text': 'x'}).states == {'seq': 9, 'menu': None, 'match_clock': 7},
      'status parser: unreadable words are None')

# ---------------------------------------------------------------- 5. plumbing
import live  # noqa: E402

sent, real_request, real_run_batch = [], live.request, live.run_batch
stream = iter(j3_stream(until=box_start + 600))


def fake_request(name, text, connect_wait=10.0):
    if text.startswith('!status'):
        s = next(stream)
        return 'status: ok\nframe: %d\nstates: seq=%d match_clock=%d\ntext: %s\n.\n' % (s.frame, s.states['seq'], s.states['match_clock'], s.text)
    return 'status: ok\nbookmark: (none)\n.\n'


def fake_run_batch(name, lines, timeout=120, shot=False, width=1280):
    sent.append(list(lines))
    return {'status': 'ok'}, [], []


live.request, live.run_batch = fake_request, fake_run_batch
said = []
try:
    g = SG.Guard('x', poll_s=0)
    for _ in range(7):
        g.tick(said.append)
finally:
    live.request, live.run_batch = real_request, real_run_batch
check(sent == [['press A']] and len(said) == 1 and said[0].startswith("[stall] pressed A: screen text '??? OK' (match_clock held at 15 for "),
      'Guard.tick presses A once through the channel and logs: %s' % (said[:1],))

src = open(os.path.join(REPO, 'tools', 'goto', 'resolve.py'), encoding='utf-8').read()
check(src.count('tick()') >= 6 and 'tick=tick' in src, 'resolve.py: wait_event, expect_log, expect_fire, the scene log and the wm30 step / fire retries tick the guard')
ev = open(os.path.join(HERE, 'events.py'), encoding='utf-8').read()
check('tick=None' in ev and 'tick()' in ev, 'events.Events.wait carries the tick')

print('\n%d problem(s)' % len(fails))
sys.exit(1 if fails else 0)
