r"""S102 lane G4b F2 (class fix): every rdr_sw::BoolSeen( / IntSeen( call must pass a GUEST frame: an argument that contains `guest_frame`.
The S0-4 table logs "[switch] frame N ..." with the guest frame; a reader that acknowledges with another counter (the binder's frame_number_) makes ab_verdict.py cut the run at the wrong frame.
  py -I tools\lint\lint_seen.py [--root DIR]        scans src\**\*.{h,cpp,inc,hpp}; exit 1 on any call without guest_frame in its arguments
  py -I tools\lint\lint_seen.py --selftest           the planted twin: a frame_number_ call must be flagged, a guest_frame call must not (exit 1 if the lint misses it)
Exempt: rdr_switches.h (the definitions and their forwarding), rdr_switches_test.cpp (literal frame numbers on a fake store)."""
import os
import re
import sys

EXEMPT = {'rdr_switches.h', 'rdr_switches_test.cpp'}
# Known other-lane sites (listed, not fixed here): (file name, switch id) -> why it is a guest frame anyway.
ALLOW = {('reader.h', 'kKoReaderNone'): "lane KO's KoLatchAtPresent: the local `frame` is wwe2k14::g_guest_frame (a guest frame under another name)"}
CALL = re.compile(r'\b(?:BoolSeen|IntSeen)\s*\(')


def calls(text):
    """Yield (line, argument text) for each BoolSeen( / IntSeen( call in `text`."""
    for m in CALL.finditer(text):
        i, depth = m.end(), 1
        while i < len(text) and depth:
            c = text[i]
            depth += (c == '(') - (c == ')')
            i += 1
        yield text.count('\n', 0, m.start()) + 1, text[m.end():i - 1]


def bad_calls(text):
    return [(ln, a) for ln, a in calls(text) if 'guest_frame' not in a]


def selftest():
    planted = 'x = rdr_sw::BoolSeen(rdr_sw::kSkipBlankPlane, g_fview.load(), frame_number_);'
    good = 'x = rdr_sw::BoolSeen(rdr_sw::kSkipBlankPlane, g_fview.load(std::memory_order_relaxed), frame.header.guest_frame);'
    nested = 'x = rdr_sw::IntSeen(rdr_sw::kMaxQueued, f(a, b), message.guest_frame) + g(frame_number_);'
    ok = len(bad_calls(planted)) == 1 and len(bad_calls(good)) == 0 and len(bad_calls(nested)) == 0
    print(('PASS' if ok else 'FAIL') + ' selftest: a frame_number_ call is flagged, a guest_frame call (also with nested parentheses) is not')
    return 0 if ok else 1


def main():
    if '--selftest' in sys.argv:
        return selftest()
    root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
    if '--root' in sys.argv:
        root = sys.argv[sys.argv.index('--root') + 1]
    src = os.path.join(root, 'src')
    if not os.path.isdir(src):
        print('FAIL cannot read', src)
        return 1
    n_files = n_calls = 0
    flagged = []
    allowed = []
    for d, _, files in os.walk(src):
        for fn in files:
            if not fn.endswith(('.h', '.cpp', '.inc', '.hpp')) or fn in EXEMPT:
                continue
            path = os.path.join(d, fn)
            text = open(path, encoding='utf-8', errors='replace').read()
            n_files += 1
            c = list(calls(text))
            n_calls += len(c)
            for ln, a in bad_calls(text):
                if any(fn == k[0] and k[1] in a for k in ALLOW):
                    allowed.append('%s:%d  %s' % (fn, ln, ALLOW[next(k for k in ALLOW if fn == k[0] and k[1] in a)]))
                    continue
                flagged.append('%s:%d  BoolSeen/IntSeen(%s)' % (os.path.relpath(path, root), ln, ' '.join(a.split())[:110]))
    if n_calls == 0:
        print('FAIL the lint found no BoolSeen / IntSeen call in %d files (it reads nothing)' % n_files)
        return 1
    for a in allowed:
        print('ALLOWED ' + a)
    for f in flagged:
        print('FLAGGED ' + f)
    print('%s %d BoolSeen / IntSeen calls in %d files, %d without a guest_frame argument' % ('FAIL' if flagged else 'PASS', n_calls, n_files, len(flagged)))
    return 1 if flagged else 0


if __name__ == '__main__':
    sys.exit(main())
