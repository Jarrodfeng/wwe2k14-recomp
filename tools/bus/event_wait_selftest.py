"""S110 BUSWAIT: the check of src/event_wait_core.h (a route's wait on the event bus). No game, no game data.

    py -I tools/bus/event_wait_selftest.py

Compiles tools/bus/event_wait_test.cpp against src/event_wait_core.h (clang++ -std=c++17) and runs it: the route line (KIND, the match
what shorthand, KEY OP VALUE fields, TIMEOUT, optional, the errors), the field reader on a bus line (strings, escapes, arrays), the matcher
(numbers, words, '_' = space, absent fields), the log (sequence numbers, a mark, two events between two reader ticks, an event never matched
twice, the ring's lost count, Last by kind and kind:what) and two threads (every seq seen once, in order).
PLANTED TWINS (a text replacement in a copy of the header, must hit exactly once, must make the test fail) are listed in TWINS.
TEXT CHECKS pin the wiring: src/event_bus.h records every Emit and turns on for a route, src/input_script.h parses wait_event / log_event.
Exit 0 = all passed.
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
HDR = os.path.join(REPO, 'src', 'event_wait_core.h')
problems = []

TWINS = [
    ('scan from the mark itself (an event matched twice)', 'uint64_t from = after + 1;', 'uint64_t from = after;'),
    ("'_' no longer matches a space", "o += c == '_' ? ' ' : char(", 'o += char('),
    ('the lost count dropped', 'lost = oldest - from;', 'lost = 0;'),
    ('numbers compared as words', 'if (ParseInt(v, a) && ParseInt(c.value, b)) {', 'if (false) {'),
    ('the poll does not move its scan point (a long wait reports read events as lost)', 'if (hit == Hit::kNone) scan = newest_;', ''),
    ('S110 SCENESRC: the until clause ignored (a wait sits out its timeout after the match ended)', 'if (until && Matches(*until, e)) {', 'if (false) {'),
    ('S110 SCENESRC: an event matching the wait and its end taken as the end', 'if (Matches(f, e)) {', 'if (Matches(f, e) && !(until && Matches(*until, e))) {'),
    ('S110 SCENESRC: the end clause dropped by the parser', 'out.until = end.filter;', ''),
    ('S110 SCENESRC: an end does not move the scan point (the end read again)', 'if (hit != Hit::kNone) scan = out.seq;', 'if (hit == Hit::kMatch) scan = out.seq;'),
    ("log_event replay WORD ignores the state", 'if ((c.key == "what" || c.key == "state") && c.op == "=")', 'if (c.key == "what" && c.op == "=")'),
]

TEXT = [
    ('src/event_bus.h', '::wwe2k14::event_wait::Global().Add(frame, kind, fields);', 'Emit records every event for the routes'),
    ('src/event_bus.h', '::wwe2k14::event_wait::g_wanted.load(', 'On() is true when a route asked for the bus'),
    ('src/input_script.h', 'event_wait::g_wanted.store(true);', 'the parser turns the bus on'),
    ('src/input_script.h', 'case Op::kWaitEvent:', 'the runner has the wait'),
    ('src/input_script.h', 'event_wait::Global().Poll(event_scan_, step.filter, step.until.kind.empty() ? nullptr : &step.until, ev, lost)',
     'the wait polls from its own scan point (review), against its until clause too (S110 SCENESRC)'),
    ('src/input_script.h', 'if (hit == event_wait::Log::Hit::kEnd) {', 'the wait ends on its until event (S110 SCENESRC)'),
    ('src/input_script.h', 'step.until = spec.until;', 'the parser keeps the until clause (S110 SCENESRC)'),
    ('src/input_script.h', 'event_scan_ = event_mark_;', 'a wait starts its scan at the mark (review)'),
    ('src/input_script.h', 'case Op::kLogEvent:', 'the runner has log_event'),
]


def check(ok, msg):
    if not ok:
        problems.append(msg)
        print('FAIL: ' + msg)


def clang():
    for c in (shutil.which('clang++'), r'C:\Program Files\LLVM\bin\clang++.exe', '/usr/bin/clang++'):
        if c and os.path.exists(c):
            return c
    raise RuntimeError('no clang++ found (tools\\env.ps1 puts the project\'s LLVM on PATH)')


def build_and_run(tmp, header, name):
    exe = os.path.join(tmp, name + '.exe')
    r = subprocess.run([clang(), '-std=c++17', '-O0', '-I', os.path.join(REPO, 'src'), '-DHDR="%s"' % header.replace('\\', '/'),
                        os.path.join(HERE, 'event_wait_test.cpp'), '-o', exe], capture_output=True, text=True, timeout=240)
    if r.returncode:
        raise RuntimeError('compile failed (%s): %s' % (name, r.stderr[-1500:]))
    try:
        r = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    except subprocess.TimeoutExpired:
        return 99, 'timed out after 60 s'
    return r.returncode, r.stdout.replace('\r\n', '\n')


def main():
    tmp = tempfile.mkdtemp(prefix='event_wait_')
    try:
        code, out = build_and_run(tmp, HDR, 'real')
        print(out.strip().splitlines()[-1] if out.strip() else '(no output)')
        check(code == 0, 'the unit failed on the real header:\n' + out)
        src = open(HDR, encoding='utf-8').read().replace('\r\n', '\n')
        for i, (what, old, new) in enumerate(TWINS):
            n = src.count(old)
            check(n == 1, 'twin %r: planted text found %d times (want 1)' % (what, n))
            if n != 1:
                continue
            p = os.path.join(tmp, 'twin%d.h' % i)
            open(p, 'w', encoding='utf-8').write(src.replace(old, new))
            tcode, tout = build_and_run(tmp, p, 'twin%d' % i)
            check(tcode != 0, 'twin %r was NOT caught by the unit' % what)
            if tcode != 0:
                print('twin caught: %s' % what)
        for path, needle, what in TEXT:
            text = open(os.path.join(REPO, path), encoding='utf-8').read()
            check(needle in text, 'text check %s: %s (%r not found)' % (path, what, needle))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print('event_wait selftest: %d problems' % len(problems))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
